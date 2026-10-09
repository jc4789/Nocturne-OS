/* 共通host fixtureだけ再利用。23のcase／fault runは実行しない。 */
#define WORKER_CLONE25
#define WORKER_BOOTSTRAP "build/goal-20261009/worker-clone25-new-once/bootstrap.js"
#define WORKER_CASES "tests/worker_clone25_new.js"
#include "worker_messaging23_new.c"
