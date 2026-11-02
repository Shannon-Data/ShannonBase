# Removes the dev-only routines installed by agent_dev_routines.inc.
#
# MTR compares the server state before and after every test; a routine left
# behind in `sys` makes that check fail, so every test that installs them must
# drop them again. The DROPs are not logged: they are bookkeeping, not output.
--disable_query_log
DROP PROCEDURE IF EXISTS sys.shannon_agent_selfcheck;
DROP PROCEDURE IF EXISTS sys.shannon_agent_loopcheck;
--enable_query_log
