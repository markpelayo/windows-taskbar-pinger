// resource.h — identifiers shared between resources.rc and the C++ source.

#pragma once

#define IDD_PROMPT        101
#define IDC_PROMPT_LABEL  1001
#define IDC_PROMPT_EDIT   1002
#define IDI_APP           201

// Command identifiers for the context menu.
//
// Ranges rather than single values where a menu is generated from a list, so an
// item's value carries its index and the handler needs no lookup table.
#define IDM_ABOUT              40001
#define IDM_SET_HOST           40002
#define IDM_TOGGLE_LATENCY     40003
#define IDM_PING_NOW           40004
#define IDM_CLEAR_HISTORY      40005
#define IDM_RESTORE_DEFAULTS   40006
#define IDM_DUPLICATE          40007
#define IDM_REMOVE             40008
#define IDM_QUIT               40009
#define IDM_SAVE_NEW_PROFILE   40010
#define IDM_INTERVAL_CUSTOM    40011
#define IDM_SUCCESS_CUSTOM     40012
#define IDM_FAILURE_CUSTOM     40013
#define IDM_MOVE_WIDGET        40014
#define IDM_RESET_POSITION     40015

#define IDM_INTERVAL_FIRST     41000   // + index into IntervalChoices
#define IDM_INTERVAL_LAST      41099

#define IDM_SUCCESS_FIRST      41100   // + index into ColorPresets
#define IDM_SUCCESS_LAST       41199

#define IDM_FAILURE_FIRST      41200
#define IDM_FAILURE_LAST       41299

#define IDM_ROWS_FIRST         41300   // + index into RowChoices
#define IDM_ROWS_LAST          41399

#define IDM_COLUMNS_FIRST      41400
#define IDM_COLUMNS_LAST       41499

#define IDM_CELL_FIRST         41500
#define IDM_CELL_LAST          41599

#define IDM_GAP_FIRST          41600
#define IDM_GAP_LAST           41699

#define IDM_TEXTSIZE_FIRST     42000   // + index into TextSizeChoices
#define IDM_TEXTSIZE_LAST      42099

#define IDM_LOAD_PROFILE_FIRST 41700   // + index into the sorted profile names
#define IDM_LOAD_PROFILE_LAST  41799

#define IDM_SAVE_PROFILE_FIRST 41800   // overwrite an existing profile
#define IDM_SAVE_PROFILE_LAST  41899

#define IDM_DEL_PROFILE_FIRST  41900   // delete a profile
#define IDM_DEL_PROFILE_LAST   41999
