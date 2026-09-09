#pragma once

// SimStatusType is mostly unused because telemetry is only recorded
// when the game is live. it's kept here in case someone
// converts the telemetry structure to time-based.
enum SimStatusType {
    STATUS_OFF = 0,
    STATUS_REPLAY = 1,
    STATUS_LIVE = 2,
    STATUS_PAUSE = 3
};

enum SimSessionType {
    SESSION_UNKNOWN = -1,
    SESSION_PRACTICE = 0,
    SESSION_QUALIFY = 1,
    SESSION_RACE = 2,
    SESSION_HOTLAP = 3,
    SESSION_TIME_ATTACK = 4,
    SESSION_DRIFT = 5,
    SESSION_DRAG = 6,
    SESSION_HOTSTINT = 7,
    SESSION_HOTSTINT_SUPERPOLE = 8
};

enum SimFlagType {
    FLAG_NONE = 0,
    FLAG_BLUE = 1,
    FLAG_YELLOW = 2,
    FLAG_BLACK = 3,
    FLAG_WHITE = 4,
    FLAG_CHECKERED = 5,
    FLAG_PENALTY = 6,
    FLAG_GREEN = 7,
    FLAG_ORANGE = 8
};

enum DrivingMistakeType {
    MISTAKE_NONE = 0,
    MISTAKE_PEDAL_OVERLAP = 1,
    MISTAKE_COASTING = 2,
    MISTAKE_ABS_ABUSE = 3,
    MISTAKE_TRAIL_BRAKING_SMOOTHNESS = 4,
    MISTAKE_SLOW_SHIFT = 5,
    MISTAKE_AGGRESSIVE_DOWNSHIFT = 6,
    MISTAKE_OVER_SLOWING = 7,
    MISTAKE_SNAP_OVERSTEER = 8,
    MISTAKE_THROTTLE_FLUTTER = 9,
    MISTAKE_LOSS_OF_CONTROL = 10,
    MISTAKE_DRIFT = 11,
    MISTAKE_MINOR_OVERSTEER = 12
};

enum CompressionMethod {
    COMPRESSION_NONE = 0,
    COMPRESSION_ZSTD = 1
};
