#ifndef PDW_MQTT_H
#define PDW_MQTT_H

// Message posted to the status window (MqttSetStatusWnd) via PostMessage.
// wParam = MHS_* constant, lParam = 0 (reserved).
#define WM_MQTT_STATUS  (WM_USER + 51)

#define MHS_IDLE     0
#define MHS_SENDING  1
#define MHS_OK       2
#define MHS_RETRY    3
#define MHS_ERROR    4
#define MHS_DISABLED 5

void MqttInit(void);
void MqttShutdown(void);
void MqttDestroy(void); // FIX [L4]: full teardown incl. DeleteCriticalSection
void MqttNotify(const char *capcode, const char *message, const char *label,
                const char *szTime, const char *szDate,
                const char *szMode, const char *szType, const char *szBitrate,
                BOOL isGroup, int groupbit);
void MqttFlushGroup(int groupbit);
void MqttSetStatusWnd(HWND hWnd);

#endif /* PDW_MQTT_H */
