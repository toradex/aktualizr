-- Don't modify this! Create a new migration instead--see docs/ota-client-guide/modules/ROOT/pages/schema-migrations.adoc
SAVEPOINT MIGRATION;

CREATE TABLE consent_config(property TEXT PRIMARY KEY, value INT NOT NULL);

DELETE FROM version;
INSERT INTO version VALUES(27);

RELEASE MIGRATION;
