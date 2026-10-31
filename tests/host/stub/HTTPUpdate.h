#pragma once
#include <Arduino.h>
#include <functional>
#include <HTTPClient.h>
enum t_httpUpdate_return { HTTP_UPDATE_FAILED, HTTP_UPDATE_NO_UPDATES, HTTP_UPDATE_OK };
struct HTTPUpdate { void rebootOnUpdate(bool); void setFollowRedirects(followRedirects_t); void onProgress(std::function<void(int,int)>);
 t_httpUpdate_return update(WiFiClient&, const String&); String getLastErrorString(); };
extern HTTPUpdate httpUpdate;
