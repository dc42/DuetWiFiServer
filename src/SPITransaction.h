// SPI transaction manager interface

#ifndef _SPITRANSACTION_H_INCLUDED
#define _SPITRANSACTION_H_INCLUDED

#include <Arduino.h>
#include <stdlib.h>

namespace SPITransaction
{
  // Transaction type field bits
  // Byte 3 (MSB) is the packet type.
  // Byte 2 holds flags
  // Byte 1 is currently unused
  // Byte 0 is the opcode if the packet is a request or info message, or the error code if it is a response.

  // Packet types
  const uint32_t trTypeRequest = 0x3A000000;          // this is a request
  const uint32_t trTypeResponse = 0xB6000000;         // this is a response to a request
  const uint32_t trTypeInfo = 0x93000000;             // this is an informational message that does not require a response

  // Flags
  const uint32_t ttDataTaken = 0x00010000;            // indicates to the SAM the the ESP8266 has read its data, and vice versa

  // Opcodes for requests from web sever to Duet
  const uint32_t ttRr = 0x01;                         // any request starting with "rr_"
  
  // Opcodes for info messages from web server to Duet
  const uint32_t ttNetworkInfo = 0x70;                // used to pass network info to Duet when first connected

  // Opcodes for requests and info from Duet to web server
  const uint32_t ttNetworkConfig = 0x80;              // set network configuration (SSID, password etc.)
  const uint32_t ttNetworkEnable = 0x81;              // enable WiFi
  const uint32_t ttGetNetworkInfo = 0x83;             // get IP address etc.

  // Opcodes for info messages from Duet to server
  const uint32_t ttMachineConfigChanged = 0x82;       // notify server that the machine configuration has changed significantly

  // Return code definitions
  const uint32_t rcNumber = 0x0000FFFF;
  const uint32_t rcJson = 0x00010000;
  const uint32_t rcKeepOpen = 0x00020000;

  // Initialise
  void Init();
  
  // Execute an SPI transaction if everything is ready
  void DoTransaction();

  // Schedule a informational message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleInfoMessage(uint32_t tt, const void *dataToSend, uint32_t length);

  // Schedule a request message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleRequestMessage(uint32_t tt, uint32_t ip, bool last, const void *dataToSend, uint32_t length);

  // Schedule a reply message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleReplyMessage(uint32_t tt, const void *dataToSend, uint32_t length);

  // Get the address of the data buffer ready to fill in postdata
  bool GetBufferAddress(uint8_t**p, size_t& length);

  // Schedule a postdata message
  void SchedulePostdataMessage(uint32_t tt, uint32_t ip, size_t length, uint32_t fragment, bool last);
  
  // Return true if we have received incoming data
  bool DataReady();

  // Get the incoming opcode and transaction type
  uint32_t GetOpcode();

  // Get the incoming fragment number
  uint32_t GetFragment(bool& isLast);

  // Get the length of incoming data and return a pointer to the data
  const void *GetData(size_t& length);

  // Flag the incoming data as taken
  void IncomingDataTaken();
};

#endif


