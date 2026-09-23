/*
 * Hardware bring-up mode (plan §4 Phase 1).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the board, start the hwtest tasks and return. Results go to the
 * log, the screen and (with "results") a Markdown table on the console. */
void hwtest_start(void);

#ifdef __cplusplus
}
#endif
