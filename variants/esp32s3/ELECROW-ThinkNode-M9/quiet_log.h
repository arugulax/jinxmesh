// Forced into every compilation of the thinknode_m9-tft-kb environment (-include in its build_flags).
// The Arduino core logs at INFO by default, and its BLE library prints the passkey of every pairing at that level
// ("Passkey: ..."). Warnings and errors only, so that no passkey reaches the serial log.
// Done here and not with a second -D: where build_unflags does not reach (the framework's own libraries, which
// are built with warnings as errors) a second definition on the command line is an error.
#undef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL ARDUHAL_LOG_LEVEL_WARN
