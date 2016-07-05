// SPI transaction manager

#include "SPITransaction.h"
#include "Config.h"
#include "HSPI.h"
#include <algorithm>

namespace SPITransaction
{
  // Transaction buffer class.
  // ***** This must be kept in step with the corresponding class in RepRapFirmwareWiFi *****
  const uint32_t maxSpiDataLength = maxSpiFileData;
  
  class TransactionBuffer
  {
      uint32_t trType;                  // type of transaction
      uint32_t seq;                     // sequence number of the request
      uint32_t ip;                      // IP address of the requester
      uint32_t fragment;                // fragment number of this packet, top bit set if last fragment
      uint32_t dataLength;              // number of bytes of data following the header
      uint32_t data[maxSpiDataLength/4];  // the actual data, if needed
      uint32_t dummy;                   // to allow us to add a null terminator to an incoming message
  
  public:
    static const uint32_t headerDwords = 5;
    static const uint32_t lastFragment = 0x80000000;

    // Initialise
    void Init();

    // Mark this buffer empty
    void Clear();

    // Return true if this buffer contains data
    bool IsReady() const
    {
      return (trType & 0xFF000000) != 0;
    }

    bool IsValid() const
    {
      return IsReady() && dataLength <= maxSpiDataLength;
    }

    // Return true if this buffer is empty
    bool IsEmpty() const
    {
      return (trType & 0xFF0000FF) == 0;      // ignore the DataTaken flag
    }

    bool DataWasTaken() const
    {
      return (trType & ttDataTaken) != 0;
    }
    
    // Get SPI packet length in dwords
    uint32_t PacketLength() const
    {
      return (IsReady()) ? (dataLength + 3)/4 + headerDwords : headerDwords;
    }

    uint32_t GetOpcode() const
    {
      return trType;
    }

    uint32_t GetFragment() const
    {
      return fragment;
    }

    const void *GetData(size_t& length) const
    {
      length = dataLength;
      return data;
    }

    void AppendNull()
    {
      if (IsReady())
      {
        *((char *)data + dataLength) = 0;
      }
    }

    // Set up a message in this buffer
    bool SetMessage(uint32_t tt, uint32_t ip, uint32_t frag, const void *dataToSend, uint32_t length);

    // Get the address and size to write data into
    bool GetBufferAddress(uint8_t**p, size_t& length)
    {
      if (IsReady())
      {
        return false;
      }
      *p = reinterpret_cast<uint8_t*>(data);
      length = maxSpiDataLength;
      // TODO reserve the buffer here so that no other client can allocate it
      return true;
    }
  };

  void TransactionBuffer::Clear()
  {
    trType = 0;
    seq = 0;
    fragment = 0;
    dataLength = 0;
    ip = 0;
  }

  bool TransactionBuffer::SetMessage(uint32_t tt, uint32_t p_ip, uint32_t frag, const void *dataToSend, uint32_t length)
  {
    if (IsReady())
    {
      return false;
    }
    trType = tt;
    seq = 0;
    fragment = frag;
    ip = p_ip;
    dataLength = length;
    if (dataToSend != nullptr)
    {
      memcpy(data, dataToSend, length);
    }
    // else if the pointer is null, we have already loaded the message in the buffer
    return true;
  }
  
  static TransactionBuffer inBuffer, outBuffer;

  static HSPIClass hspi;

  void Init()
  {
    pinMode(SamTfrReadyPin, INPUT);
    pinMode(EspReqTransferPin, OUTPUT);
    digitalWrite(EspReqTransferPin, LOW);
    pinMode(SamSSPin, OUTPUT);
    digitalWrite(SamSSPin, HIGH);

    // Set up the fast SPI channel
    hspi.begin();
    hspi.setBitOrder(MSBFIRST);
    hspi.setDataMode(SPI_MODE1);
    hspi.setFrequency(spiFrequency);

    inBuffer.Clear();
    outBuffer.Clear();
  }

  // Execute an SPI transaction if possible, by sending from outBuffer and reading any incomimg data to inBuffer.
  void DoTransaction()
  {
    if (digitalRead(SamTfrReadyPin) == HIGH && inBuffer.IsEmpty())
    {
#ifdef SPI_DEBUG
      if (outBuffer.GetOpcode() != 0)
      {
        Serial.print("Sending ");
        Serial.println(outBuffer.GetFragment());
      }
      else
      {
        Serial.println("Reading");
      }
#endif
      uint32_t dataOutLength = outBuffer.PacketLength();    // number of dwords of data to send
   
      uint32_t *inPointer = reinterpret_cast<uint32_t*>(&inBuffer);
      uint32_t *outPointer = reinterpret_cast<uint32_t*>(&outBuffer);
  
      hspi.beginTransaction();
      digitalWrite(SamSSPin, LOW);            // assert CS to SAM
      digitalWrite(EspReqTransferPin, LOW);   // stop asking to transfer data

      // Exchange headers
      hspi.transferDwords(outPointer, inPointer, TransactionBuffer::headerDwords);
      outPointer += TransactionBuffer::headerDwords;
      inPointer += TransactionBuffer::headerDwords;
      dataOutLength -= TransactionBuffer::headerDwords;

      // See if how much more data we need to read
      uint32_t dataInLength = inBuffer.PacketLength() - TransactionBuffer::headerDwords;
      if (dataInLength > maxSpiDataLength/4)
      {
        dataInLength = maxSpiDataLength/4;
        //TODO record that input has been truncated
      }

      if (dataInLength != 0 && dataOutLength != 0)
      {
        uint32_t lengthToTransfer = std::min<uint32_t>(dataInLength, dataOutLength);
        hspi.transferDwords(outPointer, inPointer, lengthToTransfer);
        inPointer += lengthToTransfer;
        outPointer += lengthToTransfer;
        dataInLength -= lengthToTransfer;
        dataOutLength -= lengthToTransfer;
      }

      if (dataInLength != 0)
      {
        hspi.transferDwords(nullptr, inPointer, dataInLength);
      }
  
      // Finished receiving, so send any remaining data
      if (dataOutLength != 0)
      {
        hspi.writeDwords(outPointer, dataOutLength);
      }

      digitalWrite(SamSSPin, HIGH);     // de-assert CS to SAM to end the transaction and tell SAM the transfer is complete
      hspi.endTransaction();

      // Check for valid data before we append a null
      if (inBuffer.IsReady())
      {
        if (inBuffer.IsValid())
        { 
          inBuffer.AppendNull();            // add a null terminator to the incoming data to simplify processing
#ifdef SPI_DEBUG
          Serial.print("Good message rec'd:");
          for (size_t i = 0; i < 10; ++i)
          {
            Serial.print(" ");
            Serial.print(*((const uint32 *)&inBuffer + i), HEX);
          }
          Serial.println();
#endif
        }
        else
        {
          Serial.print("Bad message rec'd:");
          for (size_t i = 0; i < 10; ++i)
          {
            Serial.print(" ");
            Serial.print(*((const uint32 *)&inBuffer + i), HEX);
          }
          Serial.println();
          inBuffer.Clear();
        }
      }
#ifdef SPI_DEBUG
      else
      {
        Serial.println("No message rec'd");
      }
#endif
//      if (inBuffer.DataWasTaken())
      {
        outBuffer.Clear();
      }
    }
  }

  // Schedule a informational message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleInfoMessage(uint32_t tt, const void *dataToSend, uint32_t length)
  {
    bool ok = outBuffer.SetMessage(tt | trTypeInfo, 0, TransactionBuffer::lastFragment, dataToSend, length);
    if (ok && inBuffer.IsEmpty())
    {
      digitalWrite(EspReqTransferPin, HIGH);
    }
    return ok;
  }

  // Schedule a request message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleRequestMessage(uint32_t tt, uint32_t ip, bool last, const void *dataToSend, uint32_t length)
  {
    bool ok = outBuffer.SetMessage(tt | trTypeRequest, ip, (last) ? TransactionBuffer::lastFragment : 0, dataToSend, length);
    if (ok && inBuffer.IsEmpty())
    {
      digitalWrite(EspReqTransferPin, HIGH);
    }
    return ok;
  }

  // Schedule a reply message to be sent. Returns false if there is already a message scheduled.
  bool ScheduleReplyMessage(uint32_t tt, const void *dataToSend, uint32_t length)
  {
    bool ok = outBuffer.SetMessage(tt | trTypeResponse, 0, TransactionBuffer::lastFragment, dataToSend, length);
    if (ok && inBuffer.IsEmpty())
    {
      digitalWrite(EspReqTransferPin, HIGH);
    }
    return ok;
  }

  // Get the address of the data buffer ready to fill in postdata
  bool GetBufferAddress(uint8_t**p, size_t& length)
  {
    return outBuffer.GetBufferAddress(p, length);
  }

  // Schedule a postdata message
  void SchedulePostdataMessage(uint32_t tt, uint32_t ip, size_t length, uint32_t fragment, bool last)
  {
    bool ok = outBuffer.SetMessage(trTypeRequest | tt, ip, (last) ? fragment | TransactionBuffer::lastFragment : fragment, nullptr, length);
    if (ok && inBuffer.IsEmpty())
    {
      digitalWrite(EspReqTransferPin, HIGH);
    }
  }

  // Return true if we have received incoming data
  bool DataReady()
  {
    return inBuffer.IsReady();
  }

  // Get the incoming opcode and transaction type
  uint32_t GetOpcode()
  {
    return inBuffer.GetOpcode() & 0xFF0000FF;
  }

  // Get the incoming fragment number
  uint32_t GetFragment(bool& isLast)
  {
    uint32_t fragment = inBuffer.GetFragment();
    isLast = (fragment & TransactionBuffer::lastFragment) != 0;
    return fragment & ~TransactionBuffer::lastFragment;
  }

  // Get the length of incoming data and return a pointer to the data
  const void *GetData(size_t& length)
  {
    return inBuffer.GetData(length);
  }

  // Flag the incoming data as taken
  void IncomingDataTaken()
  {
    inBuffer.Clear();
    if (outBuffer.IsReady())
    {
      digitalWrite(EspReqTransferPin, HIGH);
    }
  }

};    // end namespace

// End

