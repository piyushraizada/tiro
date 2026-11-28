#pragma once

#define IDI_ON        101
#define IDI_OFF       102
#define IDI_LOADING   103

#define IDD_ABOUT     201
#define IDD_HOTKEY    202

#define IDC_ABOUT_TEXT     1001
#define IDC_COPY           1002
#define IDC_HOTKEY_CURRENT 1010
#define IDC_HOTKEY_CAPTURE 1011
#define IDC_HOTKEY_ERROR   1012
#define IDC_CLEAR          1013

#define IDM_EXIT       40001
#define IDM_ABOUT      40002
#define IDM_HOTKEY     40003
#define IDM_EYE        40004
#define IDM_POLISH     40005
#define IDM_STARTUP    40006
#define IDM_RESTART    40007
#define IDM_MIC_DEFAULT 40100
#define IDM_MIC_FIRST  40101
#define IDM_MIC_LAST   40200
#define IDM_MODEL_FIRST 40301
#define IDM_MODEL_LAST  40400

#define IDT_LOAD       1
#define IDT_METER      3
#define IDT_EYE        4
#define IDT_WATCH      5
#define IDT_DEBUGQUIT  6   // --debug command runs: exit this many seconds after the popup
#define WM_APP_TRAY       (WM_APP + 1)
#define WM_APP_RUNNING    (WM_APP + 2)
#define WM_APP_DEVICE_LOST (WM_APP + 5)
// (WM_APP + 6 was the Escape-dismiss message for notifications; removed together
//  with the WH_KEYBOARD_LL hook that produced it - see SECURITY.md.)
#define WM_APP_FOCUS_LOST (WM_APP + 11)
#define WM_APP_ENGINE_FAIL (WM_APP + 9)
#define WM_APP_ENGINE_READY (WM_APP + 10)
#define WM_APP_POLISH       (WM_APP + 12)
#define WM_APP_POLISH_MISSING (WM_APP + 16)  // opt-in polish, optional model absent
#define HOTKEY_TOGGLE     1
#define HOTKEY_TEST       2
