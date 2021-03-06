// TacitDynDns.cpp
//
// Dynamic DNS Updater.
//
// Copyright (c) 2019, 2020 Tristan Grimmer.
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
// granted, provided that the above copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
// AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <Foundation/tVersion.h>
#include <System/tCommand.h>
#include <System/tFile.h>
#include <System/tScript.h>
#include <System/tUtil.h>
#include <System/tTime.h>
#include <Build/tProcess.h>


using namespace tStd;
using namespace tBuild;
using namespace tSystem;
tCommand::tOption Help		("Display help.", 'h', "help");
tCommand::tOption Syntax	("Display command line syntax.", 's', "syntax");
tCommand::tOption Force		("Force an update even if no change detected.", 'f', "force");
tCommand::tOption Override	("Override ipv4 or ipv6 addr. Two options allowed for both.", 'o', "override", 1);
tCommand::tParam ConfigFile	("The config file. Defaults to TacitDynDns.cfg", "ConfigFile", 1);


namespace DynDns
{
	int VersionMajor			= 1;
	int VersionMinor			= 0;
	int VersionRevision			= 2;

	//								Default
	enum class eRecord			{	IPV4,		IPV6 };
	enum class eProtocol		{	HTTPS,		HTTP };
	enum class eMode			{	Changed,	Always };

	bool IsLogNewDay(const tString& logFile);
	void ReadConfigFile(const tString& configFile);
	void ParseEnvironmentBlock(tExpr& block);
	void ParseUpdateBlock(tExpr& block);
	void ReadCurrentState();
	void UpdateAllServices();		// This does the updates. It's the workhorse.
	bool RunCurl
	(
		eProtocol protocol, const tString& username, const tString& password,
		const tString& service, const tString& domain, const tString& ipaddr
	);
	void WriteCurrentState();

	// Environment state variables.
	tString StateFile = "TacitDynDns.ips";
	tString LogFile = "TacitDynDns.log";
	enum class eLogVerbosity
	{
		// No log entries.
		None,

		// Default. One line per day max. A "-" means script was run but no updates sent.
		// A 4 means at least one ipv4 block was updated.
		// A 6 means at least one ipv6 block was updated.
		// A A means at least one ipv6 and one ipv4 block was updated.
		Concise,

		// Only actual updates.
		Minimal,

		// Updates and no-update-required entries.
		Normal,

		// Full logging.
		Full
	};
	eLogVerbosity Verbosity = eLogVerbosity::Concise;
	tString IpLookup = "ifconfig.co";
	tString Curl = "curl.exe";

	struct UpdateBlock : public tLink<UpdateBlock>
	{
		tString Domain;
		tString Service;
		eRecord Record			= eRecord::IPV4;
		eProtocol Protocol		= eProtocol::HTTPS;
		tString Username;
		tString Password;
		eMode Mode				= eMode::Changed;
		
		tString LastUpdateIP;
	};
	tList<UpdateBlock> UpdateBlocks;
	tFileHandle Log = nullptr;
	bool LogNewDay = false;
}


bool DynDns::IsLogNewDay(const tString& logFile)
{
	if (!tFileExists(logFile))
		return true;

	tFileInfo fileInfo;
	bool success = tGetFileInfo(fileInfo, logFile);
	if (!success)
		return true;

	uint64 currDayNum = tGetTimeLocal() / (10ULL * 1000000ULL * 3600ULL * 24ULL);
	uint64 logDayNum = fileInfo.ModificationTime / (10ULL * 1000000ULL * 3600ULL * 24ULL);
	return (currDayNum != logDayNum);
}


void DynDns::ReadConfigFile(const tString& configFile)
{
	tScriptReader cfg(configFile);
	tExpr block = cfg.Arg0();
	while (block.IsValid())
	{
		tString blockType = block.Item0().GetAtomString();
		if (blockType == "environment")
			DynDns::ParseEnvironmentBlock(block);

		else if (blockType == "update")
			DynDns::ParseUpdateBlock(block);

		block = block.Next();
	}
}


void DynDns::ParseEnvironmentBlock(tExpr& block)
{
	for (tExpr entry = block.Item1(); entry.IsValid(); entry = entry.Next())
	{
		if (tString(entry.Cmd()) == "statefile")
			StateFile = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "logfile")
			LogFile = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "verbosity")
		{
			tString verb = entry.Arg1().GetAtomString();
			if (verb == "none")
				Verbosity = eLogVerbosity::None;
			else if (verb == "concise")
				Verbosity = eLogVerbosity::Concise;
			else if (verb == "minimal")
				Verbosity = eLogVerbosity::Minimal;
			else if (verb == "normal")
				Verbosity = eLogVerbosity::Normal;
			else if (verb == "full")
				Verbosity = eLogVerbosity::Full;
		}

		else if (tString(entry.Cmd()) == "iplookup")
			IpLookup = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "curl")
			Curl = entry.Arg1().GetAtomString();
	}
}


void DynDns::ParseUpdateBlock(tExpr& block)
{
	UpdateBlock* update = new UpdateBlock();

	for (tExpr entry = block.Item1(); entry.IsValid(); entry = entry.Next())
	{
		if (tString(entry.Cmd()) == "domain")
			update->Domain = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "service")
			update->Service = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "record")
		{
			tString rec = entry.Arg1().GetAtomString();
			if ((rec == "ipv6") || (rec == "AAAA"))
				update->Record = eRecord::IPV6;
		}

		else if (tString(entry.Cmd()) == "protocol")
		{
			tString prot = entry.Arg1().GetAtomString();
			if (prot == "http")
				update->Protocol = eProtocol::HTTP;
		}

		else if (tString(entry.Cmd()) == "username")
			update->Username = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "password")
			update->Password = entry.Arg1().GetAtomString();

		else if (tString(entry.Cmd()) == "mode")
		{
			tString mode = entry.Arg1().GetAtomString();
			if (mode == "always")
				update->Mode = eMode::Always;
		}
	}

	UpdateBlocks.Append(update);
}


void DynDns::ReadCurrentState()
{
	if (!tFileExists(StateFile))
		return;

	tScriptReader state(StateFile);
	tExpr entry = state.Arg0();
	while (entry.IsValid())
	{
		uint64 timeStamp = entry.Item0().GetAtomUint64();
		tString domain = entry.Item1();
		eRecord record = (tString(entry.Item2()) == "ipv6") ? eRecord::IPV6 : eRecord::IPV4;
		tString ip = entry.Item3();

		for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
		{
			if ((block->Domain == domain) && (block->Record == record))
				block->LastUpdateIP = ip;
		}

		entry = entry.Next();
	}
}


void DynDns::UpdateAllServices()
{
	ulong exitCode = 0;
	tString ipv4;
	tString ipv6;

	// Are there any IP overrides?
	for (tStringItem* a = Override.Args.First(); a; a = a->Next())
	{
		if (a->CountChar('.') == 3)
			ipv4 = *a;
		else if (a->CountChar(':') == 7)
			ipv6 = *a;
	}

	if (ipv4.IsEmpty())
	{
		tProcess curlIPV4("curl -4 ifconfig.co", tGetCurrentDir(), ipv4, &exitCode);
		ipv4.Replace('\n', '\0');
		ipv4.Replace('\r', '\0');
	}
	else
	{
		if (Verbosity >= eLogVerbosity::Normal)
			ttfPrintf(Log, "Log: Using IPV4 Override of: %s\n", ipv4.Pod());
	}

	if (ipv6.IsEmpty())
	{
		tProcess curlIPV6("curl -6 ifconfig.co", tGetCurrentDir(), ipv6, &exitCode);
		ipv6.Replace('\n', '\0');
		ipv6.Replace('\r', '\0');
	}
	else
	{
		if (Verbosity >= eLogVerbosity::Normal)
			ttfPrintf(Log, "Log: Using IPV6 Override of: %s\n", ipv6.Pod());
	}

	// Are there any ipv4 blocks?
	int numIPV4Blocks = 0;
	for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
		if (block->Record == eRecord::IPV4)
			numIPV4Blocks++;

	// Update ipv4 blocks.
	bool anyUpdateIPV4 = false;
	if (ipv4.CountChar('.') == 3)
	{
		if (Verbosity >= eLogVerbosity::Full)
			ttfPrintf(Log, "Log: Detected IPV4 %s\n", ipv4.Pod());

		for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
		{
			if (block->Record != eRecord::IPV4)
				continue;

			bool attemptUpdate = Force || (block->Mode == eMode::Always) || (block->LastUpdateIP != ipv4);
			if (attemptUpdate)
			{
				bool updated = RunCurl(block->Protocol, block->Username, block->Password, block->Service, block->Domain, ipv4);
				if (updated)
				{
					if (Verbosity >= eLogVerbosity::Minimal)
						ttfPrintf(Log, "Log: Updated IPV4 %s for domain: %s\n", ipv4.Pod(), block->Domain.Pod());

					block->LastUpdateIP = ipv4;
					anyUpdateIPV4 = true;
				}
				else if (Verbosity >= eLogVerbosity::Full)
				{
					ttfPrintf(Log, "Warning: Failed running curl with IPV4 %s for domain %d.\n", ipv4.Pod(), block->Domain.Pod());
				}
			}
			else if (Verbosity >= eLogVerbosity::Normal)
			{
				ttfPrintf(Log, "Log: Skipping IPV4 %s on domain %s. No force/always and last IP equal to current.\n", ipv4.Pod(), block->Domain.Pod());
			}
		}
	}
	else if (numIPV4Blocks)
	{
		if (Verbosity >= eLogVerbosity::Full)
			ttfPrintf(Log, "Wrn: Unable to update %d IPV4 blocks. No valid IPV4 detected.\n", numIPV4Blocks);
	}

	// Are there any ipv46blocks?
	int numIPV6Blocks = 0;
	for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
		if (block->Record == eRecord::IPV6)
			numIPV6Blocks++;

	// Update ipv6 blocks.
	bool anyUpdateIPV6 = false;
	if (ipv6.CountChar(':') == 7)
	{
		if (Verbosity >= eLogVerbosity::Full)
			ttfPrintf(Log, "Log: Detected IPV6 %s\n", ipv6.Pod());

		for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
		{
			if (block->Record != eRecord::IPV6)
				continue;

			bool attemptUpdate = Force || (block->Mode == eMode::Always) || (block->LastUpdateIP != ipv6);
			if (attemptUpdate)
			{
				bool updated = RunCurl(block->Protocol, block->Username, block->Password, block->Service, block->Domain, ipv6);
				if (updated)
				{
					if (Verbosity >= eLogVerbosity::Minimal)
						ttfPrintf(Log, "Log: Updated IPV6 %s for domain: %s\n", ipv6.Pod(), block->Domain.Pod());

					block->LastUpdateIP = ipv6;
					anyUpdateIPV6 = true;
				}
				else if (Verbosity >= eLogVerbosity::Full)
				{
					ttfPrintf(Log, "Warning: Failed running curl with IPV6 %s for domain %d.\n", ipv6.Pod(), block->Domain.Pod());
				}
			}
			else if (Verbosity >= eLogVerbosity::Normal)
			{
				ttfPrintf(Log, "Log: Skipping IPV6 %s on domain %s. No force/always and last IP equal to current.\n", ipv6.Pod(), block->Domain.Pod());
			}
		}
	}
	else if (numIPV6Blocks)
	{
		if (Verbosity >= eLogVerbosity::Full)
			ttfPrintf(Log, "Wrn: Unable to update %d IPV6 blocks. No valid IPV6 detected.\n", numIPV6Blocks);
	}

	if (Verbosity == eLogVerbosity::Concise)
	{
		if (LogNewDay)
		{
			tfPrintf(Log, "\n");
			ttfPrintf(Log, "Log: Runs: ");
		}
		tString charCode = "-";
		if (anyUpdateIPV4 && anyUpdateIPV4)
			charCode = "A";
		else if (anyUpdateIPV4)
			charCode = "4";
		else if (anyUpdateIPV6)
			charCode = "6";

		tfPrintf(Log, "%s", charCode.Pod());
	}
}


bool DynDns::RunCurl
(
	eProtocol protocol, const tString& username, const tString& password,
	const tString& service, const tString& domain, const tString& ipaddr
)
{
	tString user = username;	user.Replace("@", "%40");
	tString pass = password;	pass.Replace("@", "%40");
	tString prot = (protocol == eProtocol::HTTPS) ? "HTTPS" : "HTTP";
	tString cmd;
	tsPrintf(cmd, "%s \"%s://%s:%s@%s?hostname=%s&myip=%s\"",
		Curl.Pod(), prot.Pod(), user.Pod(), pass.Pod(), service.Pod(), domain.Pod(), ipaddr.Pod());

	ulong exitCode = 0;
	tString result;
	tProcess curl(cmd, tGetCurrentDir(), result, &exitCode);
	result.Replace('\n', '\0');
	result.Replace('\r', '\0');

	// Result may be "nochg 212.34.22.489" for success/no-change or "good 212.34.22.489" for success/change.
	if (Verbosity >= eLogVerbosity::Full)
	{
		ttfPrintf(Log, "Log: Curl command: %s\n", cmd.Pod());
		ttfPrintf(Log, "Log: Curl result: Exitcode=%d Response: %s\n", exitCode, result.Pod());
	}

	bool success = false;
	if ((exitCode == 0) && ((result.FindString("good") != -1) || (result.FindString("nochg") != -1)))
		success = true;

	return success;
}


void DynDns::WriteCurrentState()
{
	tScriptWriter state(StateFile);
	state.WriteComment("TacitDynDns current state data.");
	state.NewLine();

	for (UpdateBlock* block = UpdateBlocks.First(); block; block = block->Next())
	{
		if (!block->LastUpdateIP.IsEmpty())
		{
			uint64 absTime = tGetTimeLocal();
			state.BeginExpression();
			state.WriteAtom(absTime);
			state.WriteAtom(block->Domain);
			state.WriteAtom((block->Record == eRecord::IPV4) ? "ipv4" : "ipv6");
			state.WriteAtom(block->LastUpdateIP);
			state.EndExpression();
			state.NewLine();
		}
	}
}


int main(int argc, char** argv)
{
	// If launched from a console we can attach to it. Great.
	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		WinHandle console = GetStdHandle(STD_OUTPUT_HANDLE);
		freopen("CON", "w", stdout);
	}
	tPrintf("\n");

	try
	{
		tCommand::tParse(argc, argv);
		if (Help)
		{
			tPrintf
			(
				"TacitDynDns V%d.%d.%d (2019.02) by Tristan Grimmer https://github.com/bluescan\n\n",
				DynDns::VersionMajor, DynDns::VersionMinor, DynDns::VersionRevision
			);
			tCommand::tPrintUsage();
			return 0;
		}
		else if (Syntax)
		{
			tCommand::tPrintSyntax();
			return 0;
		}

		tString configFile = "TacitDynDns.cfg";
		if (ConfigFile)
			configFile = ConfigFile.Get();

		if (!tFileExists(configFile))
		{
			tPrintf("No config file found. Default config name is TacitDynDns.cfg or specify an alternate in the command line.\n\n");
			tCommand::tPrintUsage();
			return 1;
		}

		DynDns::ReadConfigFile(configFile);
		if (DynDns::Verbosity > DynDns::eLogVerbosity::None)
		{
			DynDns::LogNewDay = DynDns::IsLogNewDay(DynDns::LogFile);
			DynDns::Log = tOpenFile(DynDns::LogFile.Pod(), "at+");
		}

		if (DynDns::Verbosity >= DynDns::eLogVerbosity::Full)
			ttfPrintf(DynDns::Log, "Log: Begin entry.\n");

		DynDns::ReadCurrentState();
		DynDns::UpdateAllServices();
		DynDns::WriteCurrentState();

		if (DynDns::Verbosity >= DynDns::eLogVerbosity::Full)
			ttfPrintf(DynDns::Log, "Log: End entry.\n");
		tSystem::tCloseFile(DynDns::Log);
	}
	catch (tError error)
	{
		if (DynDns::Verbosity >= DynDns::eLogVerbosity::Normal)
			ttfPrintf(DynDns::Log, "Error:\n%s\n", error.Message.Pod());

		tSystem::tCloseFile(DynDns::Log);
		return 1;
	}

	return 0;
}
