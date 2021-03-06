/**
 * Autogenerated by Thrift
 *
 * DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING
 */
#include "Client_types.h"

namespace Hypertable { namespace ThriftGen {

const char* RowInterval::ascii_fingerprint = "E1A4BCD94F003EFF8636F1C98591705A";
const uint8_t RowInterval::binary_fingerprint[16] = {0xE1,0xA4,0xBC,0xD9,0x4F,0x00,0x3E,0xFF,0x86,0x36,0xF1,0xC9,0x85,0x91,0x70,0x5A};

uint32_t RowInterval::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;


  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->start_row);
          this->__isset.start_row = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->start_inclusive);
          this->__isset.start_inclusive = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 3:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->end_row);
          this->__isset.end_row = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 4:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->end_inclusive);
          this->__isset.end_inclusive = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  return xfer;
}

uint32_t RowInterval::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("RowInterval");
  if (this->__isset.start_row) {
    xfer += oprot->writeFieldBegin("start_row", ::apache::thrift::protocol::T_STRING, 1);
    xfer += oprot->writeString(this->start_row);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.start_inclusive) {
    xfer += oprot->writeFieldBegin("start_inclusive", ::apache::thrift::protocol::T_BOOL, 2);
    xfer += oprot->writeBool(this->start_inclusive);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_row) {
    xfer += oprot->writeFieldBegin("end_row", ::apache::thrift::protocol::T_STRING, 3);
    xfer += oprot->writeString(this->end_row);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_inclusive) {
    xfer += oprot->writeFieldBegin("end_inclusive", ::apache::thrift::protocol::T_BOOL, 4);
    xfer += oprot->writeBool(this->end_inclusive);
    xfer += oprot->writeFieldEnd();
  }
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

const char* CellInterval::ascii_fingerprint = "D8C6D6FAE68BF8B6CA0EB2AB01E82C6C";
const uint8_t CellInterval::binary_fingerprint[16] = {0xD8,0xC6,0xD6,0xFA,0xE6,0x8B,0xF8,0xB6,0xCA,0x0E,0xB2,0xAB,0x01,0xE8,0x2C,0x6C};

uint32_t CellInterval::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;


  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->start_row);
          this->__isset.start_row = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->start_column);
          this->__isset.start_column = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 3:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->start_inclusive);
          this->__isset.start_inclusive = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 4:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->end_row);
          this->__isset.end_row = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 5:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->end_column);
          this->__isset.end_column = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 6:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->end_inclusive);
          this->__isset.end_inclusive = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  return xfer;
}

uint32_t CellInterval::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("CellInterval");
  if (this->__isset.start_row) {
    xfer += oprot->writeFieldBegin("start_row", ::apache::thrift::protocol::T_STRING, 1);
    xfer += oprot->writeString(this->start_row);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.start_column) {
    xfer += oprot->writeFieldBegin("start_column", ::apache::thrift::protocol::T_STRING, 2);
    xfer += oprot->writeString(this->start_column);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.start_inclusive) {
    xfer += oprot->writeFieldBegin("start_inclusive", ::apache::thrift::protocol::T_BOOL, 3);
    xfer += oprot->writeBool(this->start_inclusive);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_row) {
    xfer += oprot->writeFieldBegin("end_row", ::apache::thrift::protocol::T_STRING, 4);
    xfer += oprot->writeString(this->end_row);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_column) {
    xfer += oprot->writeFieldBegin("end_column", ::apache::thrift::protocol::T_STRING, 5);
    xfer += oprot->writeString(this->end_column);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_inclusive) {
    xfer += oprot->writeFieldBegin("end_inclusive", ::apache::thrift::protocol::T_BOOL, 6);
    xfer += oprot->writeBool(this->end_inclusive);
    xfer += oprot->writeFieldEnd();
  }
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

const char* ScanSpec::ascii_fingerprint = "8B2FC1E464405DA8FDD24D0B9332DCC8";
const uint8_t ScanSpec::binary_fingerprint[16] = {0x8B,0x2F,0xC1,0xE4,0x64,0x40,0x5D,0xA8,0xFD,0xD2,0x4D,0x0B,0x93,0x32,0xDC,0xC8};

uint32_t ScanSpec::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;


  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_LIST) {
          {
            this->row_intervals.clear();
            uint32_t _size0;
            ::apache::thrift::protocol::TType _etype3;
            iprot->readListBegin(_etype3, _size0);
            this->row_intervals.resize(_size0);
            uint32_t _i4;
            for (_i4 = 0; _i4 < _size0; ++_i4)
            {
              xfer += this->row_intervals[_i4].read(iprot);
            }
            iprot->readListEnd();
          }
          this->__isset.row_intervals = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_LIST) {
          {
            this->cell_intervals.clear();
            uint32_t _size5;
            ::apache::thrift::protocol::TType _etype8;
            iprot->readListBegin(_etype8, _size5);
            this->cell_intervals.resize(_size5);
            uint32_t _i9;
            for (_i9 = 0; _i9 < _size5; ++_i9)
            {
              xfer += this->cell_intervals[_i9].read(iprot);
            }
            iprot->readListEnd();
          }
          this->__isset.cell_intervals = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 3:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->return_deletes);
          this->__isset.return_deletes = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 4:
        if (ftype == ::apache::thrift::protocol::T_I32) {
          xfer += iprot->readI32(this->revs);
          this->__isset.revs = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 5:
        if (ftype == ::apache::thrift::protocol::T_I32) {
          xfer += iprot->readI32(this->row_limit);
          this->__isset.row_limit = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 6:
        if (ftype == ::apache::thrift::protocol::T_I64) {
          xfer += iprot->readI64(this->start_time);
          this->__isset.start_time = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 7:
        if (ftype == ::apache::thrift::protocol::T_I64) {
          xfer += iprot->readI64(this->end_time);
          this->__isset.end_time = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 8:
        if (ftype == ::apache::thrift::protocol::T_LIST) {
          {
            this->columns.clear();
            uint32_t _size10;
            ::apache::thrift::protocol::TType _etype13;
            iprot->readListBegin(_etype13, _size10);
            this->columns.resize(_size10);
            uint32_t _i14;
            for (_i14 = 0; _i14 < _size10; ++_i14)
            {
              xfer += iprot->readString(this->columns[_i14]);
            }
            iprot->readListEnd();
          }
          this->__isset.columns = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 9:
        if (ftype == ::apache::thrift::protocol::T_BOOL) {
          xfer += iprot->readBool(this->keys_only);
          this->__isset.keys_only = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  return xfer;
}

uint32_t ScanSpec::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("ScanSpec");
  if (this->__isset.row_intervals) {
    xfer += oprot->writeFieldBegin("row_intervals", ::apache::thrift::protocol::T_LIST, 1);
    {
      xfer += oprot->writeListBegin(::apache::thrift::protocol::T_STRUCT, this->row_intervals.size());
      std::vector<RowInterval> ::const_iterator _iter15;
      for (_iter15 = this->row_intervals.begin(); _iter15 != this->row_intervals.end(); ++_iter15)
      {
        xfer += (*_iter15).write(oprot);
      }
      xfer += oprot->writeListEnd();
    }
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.cell_intervals) {
    xfer += oprot->writeFieldBegin("cell_intervals", ::apache::thrift::protocol::T_LIST, 2);
    {
      xfer += oprot->writeListBegin(::apache::thrift::protocol::T_STRUCT, this->cell_intervals.size());
      std::vector<CellInterval> ::const_iterator _iter16;
      for (_iter16 = this->cell_intervals.begin(); _iter16 != this->cell_intervals.end(); ++_iter16)
      {
        xfer += (*_iter16).write(oprot);
      }
      xfer += oprot->writeListEnd();
    }
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.return_deletes) {
    xfer += oprot->writeFieldBegin("return_deletes", ::apache::thrift::protocol::T_BOOL, 3);
    xfer += oprot->writeBool(this->return_deletes);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.revs) {
    xfer += oprot->writeFieldBegin("revs", ::apache::thrift::protocol::T_I32, 4);
    xfer += oprot->writeI32(this->revs);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.row_limit) {
    xfer += oprot->writeFieldBegin("row_limit", ::apache::thrift::protocol::T_I32, 5);
    xfer += oprot->writeI32(this->row_limit);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.start_time) {
    xfer += oprot->writeFieldBegin("start_time", ::apache::thrift::protocol::T_I64, 6);
    xfer += oprot->writeI64(this->start_time);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.end_time) {
    xfer += oprot->writeFieldBegin("end_time", ::apache::thrift::protocol::T_I64, 7);
    xfer += oprot->writeI64(this->end_time);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.columns) {
    xfer += oprot->writeFieldBegin("columns", ::apache::thrift::protocol::T_LIST, 8);
    {
      xfer += oprot->writeListBegin(::apache::thrift::protocol::T_STRING, this->columns.size());
      std::vector<std::string> ::const_iterator _iter17;
      for (_iter17 = this->columns.begin(); _iter17 != this->columns.end(); ++_iter17)
      {
        xfer += oprot->writeString((*_iter17));
      }
      xfer += oprot->writeListEnd();
    }
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.keys_only) {
    xfer += oprot->writeFieldBegin("keys_only", ::apache::thrift::protocol::T_BOOL, 9);
    xfer += oprot->writeBool(this->keys_only);
    xfer += oprot->writeFieldEnd();
  }
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

const char* MutateSpec::ascii_fingerprint = "28C2ECC89260BADB9C70330FBF47BFA8";
const uint8_t MutateSpec::binary_fingerprint[16] = {0x28,0xC2,0xEC,0xC8,0x92,0x60,0xBA,0xDB,0x9C,0x70,0x33,0x0F,0xBF,0x47,0xBF,0xA8};

uint32_t MutateSpec::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;

  bool isset_appname = false;
  bool isset_flush_interval = false;
  bool isset_flags = false;

  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->appname);
          isset_appname = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_I32) {
          xfer += iprot->readI32(this->flush_interval);
          isset_flush_interval = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 3:
        if (ftype == ::apache::thrift::protocol::T_I32) {
          xfer += iprot->readI32(this->flags);
          isset_flags = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  if (!isset_appname)
    throw TProtocolException(TProtocolException::INVALID_DATA);
  if (!isset_flush_interval)
    throw TProtocolException(TProtocolException::INVALID_DATA);
  if (!isset_flags)
    throw TProtocolException(TProtocolException::INVALID_DATA);
  return xfer;
}

uint32_t MutateSpec::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("MutateSpec");
  xfer += oprot->writeFieldBegin("appname", ::apache::thrift::protocol::T_STRING, 1);
  xfer += oprot->writeString(this->appname);
  xfer += oprot->writeFieldEnd();
  xfer += oprot->writeFieldBegin("flush_interval", ::apache::thrift::protocol::T_I32, 2);
  xfer += oprot->writeI32(this->flush_interval);
  xfer += oprot->writeFieldEnd();
  xfer += oprot->writeFieldBegin("flags", ::apache::thrift::protocol::T_I32, 3);
  xfer += oprot->writeI32(this->flags);
  xfer += oprot->writeFieldEnd();
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

const char* Cell::ascii_fingerprint = "7D0933CA0766D7C3EAFC61FC083091CE";
const uint8_t Cell::binary_fingerprint[16] = {0x7D,0x09,0x33,0xCA,0x07,0x66,0xD7,0xC3,0xEA,0xFC,0x61,0xFC,0x08,0x30,0x91,0xCE};

uint32_t Cell::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;


  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->row_key);
          this->__isset.row_key = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->column_family);
          this->__isset.column_family = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 3:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->column_qualifier);
          this->__isset.column_qualifier = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 4:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readBinary(this->value);
          this->__isset.value = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 5:
        if (ftype == ::apache::thrift::protocol::T_I64) {
          xfer += iprot->readI64(this->timestamp);
          this->__isset.timestamp = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 6:
        if (ftype == ::apache::thrift::protocol::T_I64) {
          xfer += iprot->readI64(this->revision);
          this->__isset.revision = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 7:
        if (ftype == ::apache::thrift::protocol::T_I16) {
          xfer += iprot->readI16(this->flag);
          this->__isset.flag = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  return xfer;
}

uint32_t Cell::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("Cell");
  if (this->__isset.row_key) {
    xfer += oprot->writeFieldBegin("row_key", ::apache::thrift::protocol::T_STRING, 1);
    xfer += oprot->writeString(this->row_key);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.column_family) {
    xfer += oprot->writeFieldBegin("column_family", ::apache::thrift::protocol::T_STRING, 2);
    xfer += oprot->writeString(this->column_family);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.column_qualifier) {
    xfer += oprot->writeFieldBegin("column_qualifier", ::apache::thrift::protocol::T_STRING, 3);
    xfer += oprot->writeString(this->column_qualifier);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.value) {
    xfer += oprot->writeFieldBegin("value", ::apache::thrift::protocol::T_STRING, 4);
    xfer += oprot->writeBinary(this->value);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.timestamp) {
    xfer += oprot->writeFieldBegin("timestamp", ::apache::thrift::protocol::T_I64, 5);
    xfer += oprot->writeI64(this->timestamp);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.revision) {
    xfer += oprot->writeFieldBegin("revision", ::apache::thrift::protocol::T_I64, 6);
    xfer += oprot->writeI64(this->revision);
    xfer += oprot->writeFieldEnd();
  }
  if (this->__isset.flag) {
    xfer += oprot->writeFieldBegin("flag", ::apache::thrift::protocol::T_I16, 7);
    xfer += oprot->writeI16(this->flag);
    xfer += oprot->writeFieldEnd();
  }
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

const char* ClientException::ascii_fingerprint = "3F5FC93B338687BC7235B1AB103F47B3";
const uint8_t ClientException::binary_fingerprint[16] = {0x3F,0x5F,0xC9,0x3B,0x33,0x86,0x87,0xBC,0x72,0x35,0xB1,0xAB,0x10,0x3F,0x47,0xB3};

uint32_t ClientException::read(::apache::thrift::protocol::TProtocol* iprot) {

  uint32_t xfer = 0;
  std::string fname;
  ::apache::thrift::protocol::TType ftype;
  int16_t fid;

  xfer += iprot->readStructBegin(fname);

  using ::apache::thrift::protocol::TProtocolException;


  while (true)
  {
    xfer += iprot->readFieldBegin(fname, ftype, fid);
    if (ftype == ::apache::thrift::protocol::T_STOP) {
      break;
    }
    switch (fid)
    {
      case 1:
        if (ftype == ::apache::thrift::protocol::T_I32) {
          xfer += iprot->readI32(this->code);
          this->__isset.code = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      case 2:
        if (ftype == ::apache::thrift::protocol::T_STRING) {
          xfer += iprot->readString(this->message);
          this->__isset.message = true;
        } else {
          xfer += iprot->skip(ftype);
        }
        break;
      default:
        xfer += iprot->skip(ftype);
        break;
    }
    xfer += iprot->readFieldEnd();
  }

  xfer += iprot->readStructEnd();

  return xfer;
}

uint32_t ClientException::write(::apache::thrift::protocol::TProtocol* oprot) const {
  uint32_t xfer = 0;
  xfer += oprot->writeStructBegin("ClientException");
  xfer += oprot->writeFieldBegin("code", ::apache::thrift::protocol::T_I32, 1);
  xfer += oprot->writeI32(this->code);
  xfer += oprot->writeFieldEnd();
  xfer += oprot->writeFieldBegin("message", ::apache::thrift::protocol::T_STRING, 2);
  xfer += oprot->writeString(this->message);
  xfer += oprot->writeFieldEnd();
  xfer += oprot->writeFieldStop();
  xfer += oprot->writeStructEnd();
  return xfer;
}

}} // namespace
