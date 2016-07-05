// Configuration for RepRapWiFi

#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

//#define SPI_DEBUG
#define SHOW_PASSWORDS

// Define the maximum length (bytes) of file upload data per SPI packet. Use a multiple of the SD card file or cluster siae for efficiency.
// ************ This must be kept in step with the corresponding value in RepRapFirmwareWiFi *************
const uint32_t maxSpiFileData = 2048;

// Define the SPI clock frequency
const uint32_t spiFrequency = 40000000;

// Pin numbers
const int SamSSPin = 15;          // GPIO15, output to SAM, SS pin for SPI transfer
const int EspReqTransferPin = 0;  // GPIO0, output, indicates to the SAM that we want to send something
const int SamTfrReadyPin = 4;     // GPIO4, input, indicates that SAM is ready to execute an SPI transaction

#endif



