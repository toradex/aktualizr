-- Don't modify this! Create a new migration instead--see docs/ota-client-guide/modules/ROOT/pages/schema-migrations.adoc
SAVEPOINT MIGRATION;

ALTER TABLE device_info ADD COLUMN offline_update_path TEXT;

DELETE FROM version;
INSERT INTO version VALUES(28);

RELEASE MIGRATION;
