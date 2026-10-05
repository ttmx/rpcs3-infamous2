#include "stdafx.h"
#include "rpcs3.h"
#include "Emu/infamous2_defaults.h"

LOG_CHANNEL(sys_log, "SYS");

int main(int argc, char** argv)
{
	infamous2::apply_default_switches();

	const int exit_code = run_rpcs3(argc, argv);
	sys_log.notice("RPCS3 terminated with exit code %d", exit_code);
	return exit_code;
}
