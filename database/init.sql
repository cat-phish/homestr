-- HomeStr MariaDB schema.
-- Mirrors specs/domain-model.md. Update the spec before changing this file (SDD).
-- Executed automatically by the `db` service on first start
-- (/docker-entrypoint-initdb.d). Re-runnable: every statement is idempotent.

CREATE DATABASE IF NOT EXISTS homestr_db
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE homestr_db;

-- ---------------------------------------------------------------------------
-- Users & households
-- ---------------------------------------------------------------------------

-- `User` / `Admin` from the class diagram. Inheritance is collapsed into the
-- `role` discriminator: Admin == row with role='admin'.
CREATE TABLE IF NOT EXISTS users (
    id            VARCHAR(36)  NOT NULL PRIMARY KEY,
    name          VARCHAR(100) NOT NULL,
    email         VARCHAR(255) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,  -- bcrypt/argon2; never a plaintext password
    role          ENUM('user', 'admin') NOT NULL DEFAULT 'user',
    created_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
                               ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT uq_users_email UNIQUE (email)
) ENGINE = InnoDB;

CREATE TABLE IF NOT EXISTS households (
    id         VARCHAR(36)  NOT NULL PRIMARY KEY,
    name       VARCHAR(100) NOT NULL,
    created_at TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE = InnoDB;

-- Many-to-many: a user may belong to several households, each with a local role.
CREATE TABLE IF NOT EXISTS household_members (
    household_id VARCHAR(36) NOT NULL,
    user_id      VARCHAR(36) NOT NULL,
    member_role  ENUM('owner', 'member') NOT NULL DEFAULT 'member',
    joined_at    TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (household_id, user_id),
    CONSTRAINT fk_hm_household FOREIGN KEY (household_id)
        REFERENCES households (id) ON DELETE CASCADE,
    CONSTRAINT fk_hm_user FOREIGN KEY (user_id)
        REFERENCES users (id) ON DELETE CASCADE,
    INDEX idx_hm_user (user_id)
) ENGINE = InnoDB;

-- Opaque session tokens issued by POST /auth/login.
CREATE TABLE IF NOT EXISTS sessions (
    token_hash VARCHAR(64) NOT NULL PRIMARY KEY,  -- SHA-256 of the bearer token
    user_id    VARCHAR(36) NOT NULL,
    issued_at  TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP   NOT NULL,
    CONSTRAINT fk_sessions_user FOREIGN KEY (user_id)
        REFERENCES users (id) ON DELETE CASCADE,
    INDEX idx_sessions_user (user_id),
    INDEX idx_sessions_expiry (expires_at)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------------
-- Device hierarchy
-- ---------------------------------------------------------------------------

-- `SmartDevice` base table. Subclass attributes live in device_light /
-- device_speaker (class-table inheritance), keyed 1:1 on devices.id.
CREATE TABLE IF NOT EXISTS devices (
    id           VARCHAR(36)  NOT NULL PRIMARY KEY,
    household_id VARCHAR(36)  NULL,  -- NULL = discovered but not yet registered
    name         VARCHAR(100) NOT NULL,
    ip_address   VARCHAR(45)  NOT NULL,  -- 45 chars fits IPv6 + IPv4-mapped form
    mac_address  VARCHAR(17)  NULL,
    device_type  ENUM('light', 'speaker', 'generic') NOT NULL,
    status       ENUM('ONLINE', 'OFFLINE', 'ERROR') NOT NULL DEFAULT 'OFFLINE',
    power_on     BOOLEAN      NOT NULL DEFAULT FALSE,  -- turnOn() / turnOff()
    metadata     JSON         NULL,  -- vendor payload, firmware, capabilities
    last_seen_at TIMESTAMP    NULL,
    created_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
                              ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT fk_devices_household FOREIGN KEY (household_id)
        REFERENCES households (id) ON DELETE SET NULL,
    CONSTRAINT uq_devices_mac UNIQUE (mac_address),
    INDEX idx_devices_household (household_id),
    INDEX idx_devices_status (status)
) ENGINE = InnoDB;

-- `Light extends SmartDevice`
CREATE TABLE IF NOT EXISTS device_light (
    device_id  VARCHAR(36) NOT NULL PRIMARY KEY,
    brightness TINYINT UNSIGNED NOT NULL DEFAULT 100,
    color      CHAR(7)     NOT NULL DEFAULT '#FFFFFF',  -- #RRGGBB
    CONSTRAINT fk_light_device FOREIGN KEY (device_id)
        REFERENCES devices (id) ON DELETE CASCADE,
    CONSTRAINT ck_light_brightness CHECK (brightness <= 100),
    CONSTRAINT ck_light_color CHECK (color REGEXP '^#[0-9A-Fa-f]{6}$')
) ENGINE = InnoDB;

-- `SmartSpeaker extends SmartDevice`
CREATE TABLE IF NOT EXISTS device_speaker (
    device_id      VARCHAR(36) NOT NULL PRIMARY KEY,
    connected      BOOLEAN     NOT NULL DEFAULT FALSE,
    volume         TINYINT UNSIGNED NOT NULL DEFAULT 30,
    playback_state ENUM('PLAYING', 'PAUSED', 'STOPPED') NOT NULL DEFAULT 'STOPPED',
    current_track  VARCHAR(255) NULL,
    CONSTRAINT fk_speaker_device FOREIGN KEY (device_id)
        REFERENCES devices (id) ON DELETE CASCADE,
    CONSTRAINT ck_speaker_volume CHECK (volume <= 100)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------------
-- Automation
-- ---------------------------------------------------------------------------

-- `AutomationRule`. `trigger` is a MariaDB reserved word, hence trigger_type /
-- trigger_payload.
CREATE TABLE IF NOT EXISTS automation_rules (
    id              VARCHAR(36)  NOT NULL PRIMARY KEY,
    household_id    VARCHAR(36)  NOT NULL,
    owner_user_id   VARCHAR(36)  NULL,  -- creator; NULL once the user is deleted
    name            VARCHAR(100) NOT NULL,
    enabled         BOOLEAN      NOT NULL DEFAULT TRUE,
    trigger_type    ENUM('SCHEDULE', 'DEVICE_STATE', 'MANUAL') NOT NULL,
    trigger_payload JSON         NOT NULL,  -- e.g. {"cron":"0 22 * * *"}
    last_fired_at   TIMESTAMP    NULL,
    created_at      TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
                                 ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT fk_rules_household FOREIGN KEY (household_id)
        REFERENCES households (id) ON DELETE CASCADE,
    CONSTRAINT fk_rules_owner FOREIGN KEY (owner_user_id)
        REFERENCES users (id) ON DELETE SET NULL,
    INDEX idx_rules_household_enabled (household_id, enabled)
) ENGINE = InnoDB;

-- Ordered actions executed by AutomationRule::execute().
CREATE TABLE IF NOT EXISTS automation_actions (
    id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    rule_id        VARCHAR(36) NOT NULL,
    device_id      VARCHAR(36) NOT NULL,
    sequence_no    INT UNSIGNED NOT NULL DEFAULT 0,
    command        VARCHAR(50) NOT NULL,  -- turnOn, setBrightness, play, ...
    command_params JSON        NULL,      -- e.g. {"level":40}
    CONSTRAINT fk_actions_rule FOREIGN KEY (rule_id)
        REFERENCES automation_rules (id) ON DELETE CASCADE,
    CONSTRAINT fk_actions_device FOREIGN KEY (device_id)
        REFERENCES devices (id) ON DELETE CASCADE,
    CONSTRAINT uq_actions_order UNIQUE (rule_id, sequence_no)
) ENGINE = InnoDB;

-- Append-only audit trail of dispatched commands and state changes.
CREATE TABLE IF NOT EXISTS device_events (
    id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    device_id  VARCHAR(36) NOT NULL,
    actor_type ENUM('USER', 'RULE', 'SYSTEM') NOT NULL,
    actor_id   VARCHAR(36) NULL,  -- users.id or automation_rules.id
    command    VARCHAR(50) NOT NULL,
    payload    JSON        NULL,
    succeeded  BOOLEAN     NOT NULL DEFAULT TRUE,
    error      VARCHAR(255) NULL,
    created_at TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_events_device FOREIGN KEY (device_id)
        REFERENCES devices (id) ON DELETE CASCADE,
    INDEX idx_events_device_time (device_id, created_at)
) ENGINE = InnoDB;

-- ---------------------------------------------------------------------------
-- Seed data (development only)
-- ---------------------------------------------------------------------------

-- Placeholder hash. Rotate before any non-local deployment:
--   the backend rejects login while the hash is not a valid bcrypt digest.
INSERT INTO users (id, name, email, password_hash, role)
VALUES ('00000000-0000-0000-0000-000000000001',
        'System Administrator',
        'admin@homestr.local',
        '$2a$12$REPLACE.THIS.PLACEHOLDER.HASH.BEFORE.DEPLOYMENT.0000000',
        'admin')
ON DUPLICATE KEY UPDATE name = VALUES(name);

INSERT INTO households (id, name)
VALUES ('00000000-0000-0000-0000-0000000000a1', 'Demo Household')
ON DUPLICATE KEY UPDATE name = VALUES(name);

INSERT INTO household_members (household_id, user_id, member_role)
VALUES ('00000000-0000-0000-0000-0000000000a1',
        '00000000-0000-0000-0000-000000000001',
        'owner')
ON DUPLICATE KEY UPDATE member_role = VALUES(member_role);
