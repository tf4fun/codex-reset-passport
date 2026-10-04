#pragma once
#include "reset_history.h"

/* Shared by the actual calendar painter and host pixel probes. The compact
 * eight-week view keeps 12px labels, 14px cells and at least 6px of clear space
 * between cells; six-week builds centre the same geometry. */
#define RESET_UI_HISTORY_CELL_SIZE 14
#define RESET_UI_HISTORY_COLUMN_PITCH 21
#define RESET_UI_HISTORY_ROW_PITCH 20
#define RESET_UI_HISTORY_GROUP_OFFSET ((8 - (int)RESET_HISTORY_WEEKS) * RESET_UI_HISTORY_COLUMN_PITCH / 2)
#define RESET_UI_HISTORY_GRID_X (50 + RESET_UI_HISTORY_GROUP_OFFSET)
#define RESET_UI_HISTORY_WEEKDAY_X (26 + RESET_UI_HISTORY_GROUP_OFFSET)
#define RESET_UI_HISTORY_GRID_Y 126
#define RESET_UI_HISTORY_PANEL_X 18
#define RESET_UI_HISTORY_PANEL_Y 98
#define RESET_UI_HISTORY_PANEL_W 202
#define RESET_UI_HISTORY_PANEL_H 170
#define RESET_UI_HISTORY_MONTH_Y 104
#define RESET_UI_HISTORY_MONTH_MAX_X (RESET_UI_HISTORY_GRID_X + ((int)RESET_HISTORY_WEEKS - 1) * RESET_UI_HISTORY_COLUMN_PITCH - 12)
#define RESET_UI_HISTORY_LEGEND_Y 274
#define RESET_UI_HISTORY_SECOND_LEGEND_Y 296
