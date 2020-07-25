#pragma once

typedef enum {
  ERR_OK                    = 0,
  ERR_NOT_IMPLEMENTED       = -1,
  ERR_TIMEOUT               = -2,
  ERR_OPERATION_FAILED      = -3,
  ERR_TX                    = -4,
  ERR_RX                    = -5,
  ERR_NOT_READY             = -6,
  ERR_NO_BUFFER             = -7,
  ERR_MTU_EXCEEDED          = -8,
} error_t;
