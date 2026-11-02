# Installs the dev-only agent routines (sys.shannon_agent_selfcheck and
# sys.shannon_agent_loopcheck). They are not part of the shipped sys schema;
# see scripts/sys_schema/agent/devtools/README.md.
#
# mysqltest cannot parse the $$ quoting in the generated DDL, so the real
# client installs it. The DDL is a test fixture: `make install` puts it next to
# this file, and a build tree keeps it under scripts/sys_schema/agent/devtools.
--exec sh -c '[ -f "$MYSQL_TEST_DIR/suite/agent/include/ml_agent_dev.sql" ] && exec $MYSQL --socket="$MASTER_MYSOCK" -u root --default-character-set=utf8mb4 < "$MYSQL_TEST_DIR/suite/agent/include/ml_agent_dev.sql"; exec $MYSQL --socket="$MASTER_MYSOCK" -u root --default-character-set=utf8mb4 < "$MYSQL_BINDIR/scripts/sys_schema/agent/devtools/ml_agent_dev.sql"'
