-- Don't modify this! Create a new migration instead--see docs/ota-client-guide/modules/ROOT/pages/schema-migrations.adoc
SAVEPOINT ROLLBACK_MIGRATION;

CREATE TABLE device_info_migrate(unique_mark INTEGER PRIMARY KEY CHECK (unique_mark = 0), device_id TEXT, is_registered INTEGER NOT NULL DEFAULT 0 CHECK (is_registered IN (0,1)));
INSERT INTO device_info_migrate(unique_mark, device_id, is_registered) SELECT unique_mark, device_id, is_registered FROM device_info;
DROP TABLE device_info;
ALTER TABLE device_info_migrate RENAME TO device_info;

DELETE FROM version;
INSERT INTO version VALUES(27);

RELEASE ROLLBACK_MIGRATION;
