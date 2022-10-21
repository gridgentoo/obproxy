/**
 * Copyright (c) 2021 OceanBase
 * OceanBase Database Proxy(ODP) is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX PROXY

#include "ob_mysql_packet_util.h"
#include "common/obsm_row.h"
#include "common/obsm_utils.h"
#include "rpc/obmysql/packet/ompk_eof.h"
#include "rpc/obmysql/packet/ompk_ok.h"
#include "rpc/obmysql/packet/ompk_error.h"
#include "rpc/obmysql/packet/ompk_resheader.h"
#include "packet/ob_mysql_packet_writer.h"
#include "obproxy/engine/ob_proxy_operator_result.h"
#include "obproxy/iocore/eventsystem/ob_buf_allocator.h"

using namespace oceanbase::common;
using namespace oceanbase::obmysql;
using namespace oceanbase::obproxy::event;
using namespace oceanbase::obproxy::packet;

namespace oceanbase
{
namespace obproxy
{
int ObMysqlPacketUtil::encode_header(ObMIOBuffer &write_buf,
                                     uint8_t &seq,
                                     ObIArray<ObMySQLField> &fields,
                                     uint16_t status_flag /* 0 */)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(fields.count() < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument, field array has no element", K(ret));
  } else {
    // write header packet
    OMPKResheader rhp;
    rhp.set_field_count(fields.count());
    rhp.set_seq(seq++);
    if (OB_FAIL(ObMysqlPacketWriter::write_packet(write_buf, rhp))) {
      seq--;
      LOG_WARN("fail to write packet", K(rhp), K(ret));
    }
    // write field packet(s)
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; (OB_SUCC(ret) && i < fields.count()); ++i) {
        ObMySQLField &field = fields.at(i);
        OMPKField field_packet(field);
        field_packet.set_seq(seq++);
        if (OB_FAIL(ObMysqlPacketWriter::write_field_packet(write_buf, field_packet))) {
          LOG_WARN("fail to write field", K(field_packet), K(field), K(ret));
          --seq;
        }
      }
    }
    // write the first eof packet
    if (OB_SUCC(ret)) {
      if (OB_FAIL(encode_eof_packet(write_buf, seq, status_flag))) {
        LOG_WARN("fail to write eof packet", K(ret));
      }
    }
  }
  return ret;
}

int ObMysqlPacketUtil::encode_row_packet(ObMIOBuffer &write_buf,
                                         uint8_t &seq,
                                         const ObNewRow &row,
                                         ObIArray<ObField> *fields)
{
  int ret = OB_SUCCESS;

  ObSMRow sm_row(TEXT, row, NULL, fields);
  OMPKRow row_packet(sm_row);
  row_packet.set_seq(seq++);

  if (OB_FAIL(ObMysqlPacketWriter::write_row_packet(write_buf, row_packet))) {
    LOG_WARN("fail to write field", K(row_packet), K(ret));
    --seq;
  }
  return ret;
}

int ObMysqlPacketUtil::encode_eof_packet(ObMIOBuffer &write_buf,
                                         uint8_t &seq,
                                         uint16_t status_flag /* 0 */)
{
  int ret = OB_SUCCESS;

  OMPKEOF eof_packet;
  eof_packet.set_warning_count(0);
  eof_packet.set_seq(seq++);
  ObServerStatusFlags server_status(status_flag);
  eof_packet.set_server_status(server_status);

  if (OB_FAIL(ObMysqlPacketWriter::write_packet(write_buf, eof_packet))) {
    LOG_WARN("fail to write eof packet", K(eof_packet), K(ret));
  }
  return ret;
}

int ObMysqlPacketUtil::encode_executor_response_packet(ObMIOBuffer *write_buf, uint8_t &seq,
                                                       engine::ObProxyResultResp *result_resp)
{
  int ret = OB_SUCCESS;

  int64_t column_count = result_resp->get_column_count();
  ObSEArray<ObMySQLField, 1, common::ObIAllocator&> *fields = NULL;
  ObSEArray<ObField, 4> ob_fields;

  // header , cols , first eof
  if (OB_SUCC(ret)) {
    if (OB_FAIL(result_resp->get_fields(fields))) {
      LOG_WARN("faild to push field", K(fields), K(ret));
    } else if (OB_ISNULL(fields)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fields should not be null", K(ret));
    } else if (OB_UNLIKELY(fields->count() != column_count)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fields count should equal column_count", K(column_count), "fileds count", fields->count(), K(ret));
    } else if (OB_FAIL(ObMysqlPacketUtil::encode_header(*write_buf, seq, *fields))) {
      LOG_WARN("faild to encode header", K(fields), K(seq), K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; i++) {
        ObMySQLField &mysql_field = fields->at(i);
        ObField ob_field;
        if (OB_FAIL(ObSMUtils::to_ob_field(mysql_field, ob_field))) {
          LOG_WARN("faild to ob field", K(mysql_field), K(ret));
        } else if (OB_FAIL(ob_fields.push_back(ob_field))) {
          LOG_WARN("faild to push ob field", K(ob_field), K(ret));
        }
      }
    }
  }

  // rows
  if (OB_SUCC(ret)) {
    ObObj *objs = NULL;
    int64_t buf_len = sizeof(ObObj) * column_count;

    if (OB_ISNULL(objs = static_cast<ObObj *>(op_fixed_mem_alloc(buf_len)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("faild to alloc obj array", K(column_count), K(ret));
    } else {
      ObNewRow row;
      //ObSEArray<common::ObObj, 4> *row_array;
      ObSEArray<common::ObObj*, 4, common::ObIAllocator&> *row_array;
      while ((OB_SUCC(ret)) && (OB_SUCC(result_resp->next(row_array)))) {
        if (OB_ISNULL(row_array)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("row_array should not be null", K(ret));
        } else if (OB_UNLIKELY(row_array->count() != column_count)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("row_array count should equal column_count", K(column_count),
                   "row_array count", row_array->count(), K(ret));
        }

        for (int64_t i = 0; OB_SUCC(ret) && i < column_count; i++) {
          objs[i] = *(row_array->at(i));
        }

        if (OB_SUCC(ret)) {
          row.cells_ = objs;
          row.count_ = column_count;
          if (OB_FAIL(ObMysqlPacketUtil::encode_row_packet(*write_buf, seq, row, &ob_fields))) {
            LOG_WARN("faild to encode row", K(seq), K(row), K(ret));
          } else {
            row.reset();
            row_array = NULL;
          }
        }
      }

      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }

    if (OB_NOT_NULL(objs)) {
      op_fixed_mem_free(objs, buf_len);
      objs = NULL;
    }
  }

  // second eof
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObMysqlPacketUtil::encode_eof_packet(*write_buf, seq))) {
      LOG_WARN("faild to encode row", K(seq), K(ret));
    }
  }

  return ret;
}

int ObMysqlPacketUtil::encode_err_packet(ObMIOBuffer &write_buf, uint8_t &seq, const int errcode, const char *msg_buf)
{
  return encode_err_packet(write_buf, seq, errcode, ObString::make_string(msg_buf));
}

int ObMysqlPacketUtil::encode_err_packet(ObMIOBuffer &write_buf, uint8_t &seq, const int errcode,
                                         const ObString &msg_buf)
{
  int ret = OB_SUCCESS;

  OMPKError err_packet;
  err_packet.set_seq(seq++);
  if (OB_SUCC(err_packet.set_oberrcode(errcode))
      && OB_SUCC(err_packet.set_message(msg_buf))) {
    if (OB_FAIL(ObMysqlPacketWriter::write_packet(write_buf, err_packet))) {
      LOG_WARN("fail to write err packet", K(err_packet), K(ret));
    }
  } else {
    LOG_WARN("failed to set error info", K(ret), K(errcode), K(msg_buf));
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObMysqlPacketUtil::encode_ok_packet(ObMIOBuffer &write_buf, uint8_t &seq,
                                        const int64_t affected_rows,
                                        const ObMySQLCapabilityFlags &capability,
                                        const uint16_t status_flag /* 0 */)
{
  int ret = OB_SUCCESS;

  OMPKOK ok_packet;
  ok_packet.set_seq(seq++);
  ok_packet.set_affected_rows(static_cast<uint64_t>(affected_rows));
  ok_packet.set_capability(capability);
  ObServerStatusFlags server_status(status_flag);
  ok_packet.set_server_status(server_status);

  if (OB_FAIL(ObMysqlPacketWriter::write_packet(write_buf, ok_packet))) {
    seq--;
    LOG_WARN("fail to write packet", K(ok_packet), K(ret));
  }
  return ret;
}

int ObMysqlPacketUtil::encode_kv_resultset(ObMIOBuffer &write_buf,
                                           uint8_t &seq,
                                           const ObMySQLField &field,
                                           ObObj &field_value,
                                           const uint16_t status_flag)
{
  int ret = OB_SUCCESS;

  // header , cols , first eof
  if (OB_SUCC(ret)) {
    ObSEArray<ObMySQLField, 1> fields;
    if (OB_FAIL(fields.push_back(field))) {
      LOG_WARN("faild to push field", K(field), K(ret));
    } else if (OB_FAIL(ObMysqlPacketUtil::encode_header(write_buf, seq, fields, status_flag))) {
      LOG_WARN("faild to encode header", K(field), K(seq), K(status_flag), K(ret));
    }
  }

  // rows
  if (OB_SUCC(ret)) {
    ObNewRow row;
    row.cells_ = &field_value;
    row.count_ = 1;
    if (OB_FAIL(ObMysqlPacketUtil::encode_row_packet(write_buf, seq, row))) {
      LOG_WARN("faild to encode row", K(row), K(ret));
    }
  }

  // second eof
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObMysqlPacketUtil::encode_eof_packet(write_buf, seq, status_flag))) {
      LOG_WARN("faild to encode row", K(seq), K(status_flag), K(ret));
    }
  }

  return ret;
}
}//end of namespace obproxy
}//end of namespace oceanbase
