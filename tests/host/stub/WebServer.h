#pragma once
#include <Arduino.h>
#include <functional>
enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload { HTTPUploadStatus status; uint8_t buf[2048]; size_t currentSize; };
enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST, HTTP_DELETE };
struct WebServer { WebServer(int); typedef std::function<void(void)> THandlerFunction;
 void on(const String&, HTTPMethod, THandlerFunction); void on(const String&, HTTPMethod, THandlerFunction, THandlerFunction); HTTPUpload& upload(); void onNotFound(THandlerFunction); void begin();
 void handleClient(); String arg(const String&); String header(const String&); HTTPMethod method();
 void send(int, const char*, const String&); void send_P(int, const char*, const char*); void collectHeaders(const char**, size_t); void sendHeader(const String&, const String&); void stop(); };
