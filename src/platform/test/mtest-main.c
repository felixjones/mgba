/* Copyright (c) 2022 Felix Jones
*
* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/gba.h>

#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <signal.h>

#define MTEST_OPTIONS "S:R:N"
#define MTEST_USAGE \
	"\nAdditional options:\n" \
	"  -S SWI       	Run until specified SWI call before exiting\n" \
	"  -R REGISTER      Register to return as exit code\n" \
	"  -N               Disable video rendering entirely\n" \

 enum mRegisterExitCode {
	 mRegister_NONE = -1,
	 mRegister_GPRS = 0,
	 mRegister_CPSR = 16
 };

struct MTestOpts {
	int exitSwiImmediate;
	enum mRegisterExitCode returnCodeRegister;
	bool noVideo;
};

static int _mTestRunloop(struct mCore* core, int exitSwiImmediate, enum mRegisterExitCode registerExitCode);
static void _mTestShutdown(int signal);
static bool _parseMTestOpts(struct mSubParser* parser, int option, const char* arg);
static enum mRegisterExitCode _parseNamedRegister(const char* regStr);

static bool _dispatchExiting = false;

#ifdef M_CORE_GBA
void (*_armSwi16)(struct ARMCore* cpu, int immediate);
void (*_armSwi32)(struct ARMCore* cpu, int immediate);

static void _mTestSwi16(struct ARMCore* cpu, int immediate);
static void _mTestSwi32(struct ARMCore* cpu, int immediate);

static int _prevSwiImmediate = -1;
#endif

int main(int argc, char * argv[]) {
	signal(SIGINT, _mTestShutdown);

	struct MTestOpts mTestOpts = { -1, mRegister_NONE, false };
	struct mSubParser subparser = {
		.usage = MTEST_USAGE,
		.parse = _parseMTestOpts,
		.extraOptions = MTEST_OPTIONS,
		.opts = &mTestOpts
	};

	struct mArguments args;
	bool parsed = parseArguments(&args, argc, argv, &subparser);
	if (!args.fname) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], MTEST_USAGE);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		return 0;
	}
	struct mCore* core = mCoreFind(args.fname);
	if (!core) {
		return 1;
	}
	core->init(core);
	mCoreInitConfig(core, "mTest");
	applyArguments(&args, NULL, &core->config);

	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "remove");

	void* outputBuffer;
	outputBuffer = 0;

	if (!mTestOpts.noVideo) {
		outputBuffer = malloc(256 * 256 * 4);
		core->setVideoBuffer(core, outputBuffer, 256);
	}

#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		((struct GBA*) core->board)->hardCrash = false;

		_armSwi16 = ((struct GBA*) core->board)->cpu->irqh.swi16;
		((struct GBA*) core->board)->cpu->irqh.swi16 = _mTestSwi16;
		_armSwi32 = ((struct GBA*) core->board)->cpu->irqh.swi32;
		((struct GBA*) core->board)->cpu->irqh.swi32 = _mTestSwi32;
	}
#endif

	bool cleanExit = true;
	if (!mCoreLoadFile(core, args.fname)) {
		cleanExit = false;
		goto loadError;
	}
	if (args.patch) {
		core->loadPatch(core, VFileOpen(args.patch, O_RDONLY));
	}

	struct VFile* savestate = NULL;

	if (args.savestate) {
		savestate = VFileOpen(args.savestate, O_RDONLY);
	}

	core->reset(core);

	struct mCheatDevice* device;
	if (args.cheatsFile && (device = core->cheatDevice(core))) {
		struct VFile* vf = VFileOpen(args.cheatsFile, O_RDONLY);
		if (vf) {
			mCheatDeviceClear(device);
			mCheatParseFile(device, vf);
			vf->close(vf);
		}
	}

	if (savestate) {
		mCoreLoadStateNamed(core, savestate, 0);
		savestate->close(savestate);
		savestate = 0;
	}

	int returnCode = _mTestRunloop(core, mTestOpts.exitSwiImmediate, mTestOpts.returnCodeRegister);

	core->unloadROM(core);

	if (savestate) {
		savestate->close(savestate);
	}

loadError:
	freeArguments(&args);
	if (outputBuffer) {
		free(outputBuffer);
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);

	return cleanExit ? returnCode : 1;
}

static int _mTestRunloop(struct mCore* core, int exitSwiImmediate, enum mRegisterExitCode registerExitCode) {
	do {
		core->step(core);
	} while (_prevSwiImmediate != exitSwiImmediate && !_dispatchExiting);

#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		if (registerExitCode >= 0 && registerExitCode < 16) {
			return ((struct GBA*) core->board)->cpu->regs.gprs[registerExitCode];
		}
		if (registerExitCode == mRegister_CPSR) {
			return ((struct GBA*) core->board)->cpu->regs.cpsr.packed;
		}
	}
#endif
	return 0;
}

static void _mTestShutdown(int signal) {
	UNUSED(signal);
	_dispatchExiting = true;
}

#ifdef M_CORE_GBA
static void _mTestSwi16(struct ARMCore* cpu, int immediate) {
	_armSwi16(cpu, immediate);
	_prevSwiImmediate = immediate;
}

static void _mTestSwi32(struct ARMCore* cpu, int immediate) {
	_armSwi32(cpu, immediate);
	_prevSwiImmediate = immediate;
}
#endif

static bool _parseMTestOpts(struct mSubParser* parser, int option, const char* arg) {
	struct MTestOpts* opts = parser->opts;
	errno = 0;
	switch (option) {
	case 'S':
		opts->exitSwiImmediate = strtol(arg, NULL, 0);
		return !errno;
	case 'R':
		opts->returnCodeRegister = _parseNamedRegister(arg);
		return !errno;
	case 'N':
		opts->noVideo = true;
		return true;
	default:
		return false;
	}
}

static enum mRegisterExitCode _parseNamedRegister(const char* regStr) {
	if (regStr[0] == 'r' || regStr[0] == 'R') {
		return mRegister_GPRS + strtol(regStr + 1, NULL, 10);
	}

	if (0 == strcasecmp("sp", regStr)) {
		return mRegister_GPRS + 13;
	}
	if (0 == strcasecmp("lr", regStr)) {
		return mRegister_GPRS + 14;
	}
	if (0 == strcasecmp("pc", regStr)) {
		return mRegister_GPRS + 15;
	}

	if (0 == strcasecmp("cpsr", regStr)) {
		return mRegister_CPSR;
	}

	return mRegister_NONE;
}