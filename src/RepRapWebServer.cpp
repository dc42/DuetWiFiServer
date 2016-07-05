/*
  ESP8266WebServer.cpp - Dead simple web-server.
  Supports only one simultaneous client, knows how to handle GET and POST.

  Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)
*/


#include <Arduino.h>
#include "WiFiServer.h"
#include "WiFiClient.h"
#include "RepRapWebServer.h"
#include "FS.h"
#include "RequestHandlersImpl.h"
// #define DEBUG
#define DEBUG_OUTPUT Serial


RepRapWebServer::RepRapWebServer(IPAddress addr, int port)
: _server(addr, port)
, _currentMethod(HTTP_ANY)
, _currentHandler(0)
, _firstHandler(0)
, _lastHandler(0)
, _currentArgCount(0)
, _currentArgs(0)
, _headerKeysCount(0)
, _currentHeaders(0)
, _contentLength(0)
, _postLength(0)
, _servingPrinter(false)
{
}

RepRapWebServer::RepRapWebServer(int port)
: _server(port)
, _currentMethod(HTTP_ANY)
, _currentHandler(0)
, _firstHandler(0)
, _lastHandler(0)
, _currentArgCount(0)
, _currentArgs(0)
, _headerKeysCount(0)
, _currentHeaders(0)
, _contentLength(0)
, _postLength(0)
, _servingPrinter(false)
{
}

RepRapWebServer::~RepRapWebServer() {
  if (_currentHeaders)
    delete[]_currentHeaders;
  _headerKeysCount = 0;
  RequestHandler* handler = _firstHandler;
  while (handler) {
    RequestHandler* next = handler->next();
    delete handler;
    handler = next;
  }
}

void RepRapWebServer::begin() {
  _server.begin();
}

void RepRapWebServer::on(const char* uri, RepRapWebServer::THandlerFunction handler) {
  on(uri, HTTP_ANY, handler);
}

void RepRapWebServer::on(const char* uri, HTTPMethod method, RepRapWebServer::THandlerFunction fn) {
  on(uri, method, fn, _fileUploadHandler);
}

void RepRapWebServer::on(const char* uri, HTTPMethod method, RepRapWebServer::THandlerFunction fn, RepRapWebServer::THandlerFunction ufn) {
  _addRequestHandler(new FunctionRequestHandler(fn, ufn, uri, method));
}

void RepRapWebServer::onPrefix(const char* prefix, HTTPMethod method, RepRapWebServer::THandlerFunction fn, RepRapWebServer::THandlerFunction ufn) {
  _addRequestHandler(new PrefixRequestHandler(fn, ufn, prefix, method));
}

void RepRapWebServer::addHandler(RequestHandler* handler) {
    _addRequestHandler(handler);
}

void RepRapWebServer::_addRequestHandler(RequestHandler* handler) {
    if (!_lastHandler) {
      _firstHandler = handler;
      _lastHandler = handler;
    }
    else {
      _lastHandler->next(handler);
      _lastHandler = handler;
    }
}

void RepRapWebServer::serveStatic(const char* uri, FS& fs, const char* path, const char* cache_header) {
    _addRequestHandler(new StaticRequestHandler(fs, path, uri, cache_header));
}

void RepRapWebServer::handleClient() {
  WiFiClient client = _server.available();
  if (!client) {
    return;
  }

#ifdef DEBUG
  DEBUG_OUTPUT.println("New client");
#endif

  // Wait for data from client to become available
  uint16_t maxWait = HTTP_MAX_DATA_WAIT;
  while(client.connected() && !client.available() && maxWait--){
    delay(1);
  }

  size_t postLength;
  if (!_parseRequest(client, postLength)) {
    return;
  }

  _currentClient = client;
  _postLength = postLength;
  _contentLength = CONTENT_LENGTH_NOT_SET;
  _handleRequest();
  if (postLength != 0)
  {
    client.flush();
  }
}

void RepRapWebServer::sendHeader(const String& name, const String& value, bool first) {
  String headerLine = name;
  headerLine += ": ";
  headerLine += value;
  headerLine += "\r\n";

  if (first) {
    _responseHeaders = headerLine + _responseHeaders;
  }
  else {
    _responseHeaders += headerLine;
  }
}

void RepRapWebServer::_prepareHeader(String& response, int code, const char* content_type, size_t contentLength)
{
    response = "HTTP/1.1 ";
    response += String(code);
    response += " ";
    response += _responseCodeToString(code);
    response += "\r\nCache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n";

    if (!content_type)
    {
        content_type = "text/html";
    }
    sendHeader("Content-Type", content_type, true);

    if (_contentLength != CONTENT_LENGTH_UNKNOWN && _contentLength != CONTENT_LENGTH_NOT_SET)
    {
        sendHeader("Content-Length", String(_contentLength));
    }
    else
    {
        sendHeader("Content-Length", String(contentLength));
    }

    response += _responseHeaders;
    response += "Connection: close\r\n\r\n";
    _responseHeaders = String();
}

void RepRapWebServer::send(int code, size_t contentLength, const __FlashStringHelper *contentType, const uint8_t *data, size_t dataLength, bool isLast)
{
    String header;
    String contentTypeStr(contentType);
    _prepareHeader(header, code, contentTypeStr.c_str(), contentLength);
    sendContent(header, isLast && dataLength == 0);
    if (dataLength != 0)
    {
      sendContent(data, dataLength, isLast);
    }
}

void RepRapWebServer::sendMore(const uint8_t *data, size_t dataLength, bool isLast)
{
    sendContent(data, dataLength, isLast);    
}

void RepRapWebServer::send(int code, const char* content_type, const String& content)
{
    String header;
    _prepareHeader(header, code, content_type, content.length());
    sendContent(header, false);
    sendContent(content, true);
}

void RepRapWebServer::send_P(int code, PGM_P content_type, PGM_P content)
{
    size_t contentLength = 0;

    if (content != NULL)
    {
        contentLength = strlen_P(content);
    }

    String header;
    char type[64];
    memccpy_P((void*)type, (PGM_VOID_P)content_type, 0, sizeof(type));
    _prepareHeader(header, code, (const char* )type, contentLength);
    sendContent(header, false);
    sendContent_P(content, true);
}

void RepRapWebServer::send_P(int code, PGM_P content_type, PGM_P content, size_t contentLength) {
    String header;
    char type[64];
    memccpy_P((void*)type, (PGM_VOID_P)content_type, 0, sizeof(type));
    _prepareHeader(header, code, (const char* )type, contentLength);
    sendContent(header, false);
    sendContent_P(content, contentLength, true);
}

void RepRapWebServer::send(int code, char* content_type, const String& content)
{
  send(code, (const char*)content_type, content);
}

void RepRapWebServer::send(int code, const String& content_type, const String& content)
{
  send(code, (const char*)content_type.c_str(), content);
}

void RepRapWebServer::sendContent(const uint8_t *content, size_t dataLength, bool last)
{
  const size_t unit_size = HTTP_DOWNLOAD_UNIT_SIZE;

  while (dataLength != 0)
  {
    size_t will_send = (dataLength < unit_size) ? dataLength : unit_size;
    size_t sent = _currentClient.write(content, will_send, last && will_send == dataLength);
    if (sent == 0)
    {
      break;
    }
    dataLength -= sent;
    content += sent;
  }  
//  if (isLast)
//  {
//    _currentClient.close();
//  }
}

void RepRapWebServer::sendContent(const String& content, bool last)
{
  sendContent((const uint8_t*)content.c_str(), content.length(), last);
}

void RepRapWebServer::sendContent_P(PGM_P content, bool last)
{
    char contentUnit[HTTP_DOWNLOAD_UNIT_SIZE + 1];

    contentUnit[HTTP_DOWNLOAD_UNIT_SIZE] = '\0';

    while (content != NULL) {
        size_t contentUnitLen;
        PGM_P contentNext;

        // due to the memccpy signature, lots of casts are needed
        contentNext = (PGM_P)memccpy_P((void*)contentUnit, (PGM_VOID_P)content, 0, HTTP_DOWNLOAD_UNIT_SIZE);

        if (contentNext == NULL) {
            // no terminator, more data available
            content += HTTP_DOWNLOAD_UNIT_SIZE;
            contentUnitLen = HTTP_DOWNLOAD_UNIT_SIZE;
        }
        else {
            // reached terminator. Do not send the terminator
            contentUnitLen = contentNext - contentUnit - 1;
            content = NULL;
        }

        // write is so overloaded, had to use the cast to get it pick the right one
        _currentClient.write((const uint8_t*)contentUnit, contentUnitLen, last && content == NULL);
    }
//    if (isLast)
//    {
//      _currentClient.close();
//    }
}

void RepRapWebServer::sendContent_P(PGM_P content, size_t size, bool last)
{
    char contentUnit[HTTP_DOWNLOAD_UNIT_SIZE + 1];
    contentUnit[HTTP_DOWNLOAD_UNIT_SIZE] = '\0';
    size_t remaining_size = size;

    while (content != NULL && remaining_size > 0) {
        size_t contentUnitLen = HTTP_DOWNLOAD_UNIT_SIZE;

        if (remaining_size < HTTP_DOWNLOAD_UNIT_SIZE) contentUnitLen = remaining_size;
        // due to the memcpy signature, lots of casts are needed
        memcpy_P((void*)contentUnit, (PGM_VOID_P)content, contentUnitLen);

        content += contentUnitLen;
        remaining_size -= contentUnitLen;

        // write is so overloaded, had to use the cast to get it pick the right one
        _currentClient.write((const uint8_t*)contentUnit, contentUnitLen, last && remaining_size == 0);
    }
//    if (isLast)
//    {
//      _currentClient.close();
//    }
}

String RepRapWebServer::arg(const char* name) {
  for (int i = 0; i < _currentArgCount; ++i) {
    if (_currentArgs[i].key == name)
      return _currentArgs[i].value;
  }
  return String();
}

String RepRapWebServer::arg(int i) {
  if (i < _currentArgCount)
    return _currentArgs[i].value;
  return String();
}

String RepRapWebServer::argName(int i) {
  if (i < _currentArgCount)
    return _currentArgs[i].key;
  return String();
}

int RepRapWebServer::args() {
  return _currentArgCount;
}

bool RepRapWebServer::hasArg(const char* name) {
  for (int i = 0; i < _currentArgCount; ++i) {
    if (_currentArgs[i].key == name)
      return true;
  }
  return false;
}

String RepRapWebServer::header(const char* name) {
  for (int i = 0; i < _headerKeysCount; ++i) {
    if (_currentHeaders[i].key == name)
      return _currentHeaders[i].value;
  }
  return String();
}

void RepRapWebServer::collectHeaders(const char* headerKeys[], const size_t headerKeysCount) {
  _headerKeysCount = headerKeysCount;
  if (_currentHeaders)
     delete[]_currentHeaders;
  _currentHeaders = new RequestArgument[_headerKeysCount];
  for (int i = 0; i < _headerKeysCount; i++){
    _currentHeaders[i].key = headerKeys[i];
  }
}

String RepRapWebServer::header(int i) {
  if (i < _headerKeysCount)
    return _currentHeaders[i].value;
  return String();
}

String RepRapWebServer::headerName(int i) {
  if (i < _headerKeysCount)
    return _currentHeaders[i].key;
  return String();
}

int RepRapWebServer::headers() {
  return _headerKeysCount;
}

bool RepRapWebServer::hasHeader(const char* name) {
  for (int i = 0; i < _headerKeysCount; ++i) {
    if ((_currentHeaders[i].key == name) &&  (_currentHeaders[i].value.length() > 0))
      return true;
  }
  return false;
}

String RepRapWebServer::hostHeader() {
  return _hostHeader;
}

void RepRapWebServer::onFileUpload(THandlerFunction fn) {
  _fileUploadHandler = fn;
}

void RepRapWebServer::onNotFound(THandlerFunction fn) {
  _notFoundHandler = fn;
}

void RepRapWebServer::_handleRequest() {
  bool handled = false;
  if (!_currentHandler){
#ifdef DEBUG
    DEBUG_OUTPUT.println("request handler not found");
#endif
  }
  else {
    handled = _currentHandler->handle(*this, _currentMethod, _currentUri);
#ifdef DEBUG
    if (!handled) {
      DEBUG_OUTPUT.println("request handler failed to handle request");
    }
#endif
  }

  if (!handled) {
    if(_notFoundHandler) {
      _notFoundHandler();
    }
    else {
      send(404, "text/plain", String("Not found: ") + _currentUri);
    }
  }

  uint16_t maxWait = HTTP_MAX_CLOSE_WAIT;
  while(_currentClient.connected() && maxWait--) {
    delay(1);
  }
  _currentClient   = WiFiClient();
  _currentUri      = String();
}

const char* RepRapWebServer::_responseCodeToString(int code) {
  switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Time-out";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Request Entity Too Large";
    case 414: return "Request-URI Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Requested range not satisfiable";
    case 417: return "Expectation Failed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Time-out";
    case 505: return "HTTP Version not supported";
    default:  return "";
  }
}
