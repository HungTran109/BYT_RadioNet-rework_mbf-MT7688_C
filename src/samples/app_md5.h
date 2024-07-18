/**
 @file		md5.h
 */

#ifndef __MD5_H
#define __MD5_H
#include <stdint.h>
//typedef unsigned char uint8_t ;
//typedef signed char int8 ;
//typedef unsigned short uint16 ;
//typedef signed short int16 ;
//typedef unsigned int uint32_t_t_t ;
//typedef signed int int32 ;

/**
 @brief	MD5 context. 
 */
typedef struct {
        uint32_t state[4];    /**< state (ABCD)                            */
        uint32_t count[2];    /**< number of bits, modulo 2^64 (lsb first) */
        uint8_t  buffer[64];  /**< input buffer                            */
      } app_md5_ctx;

void app_md5_init(app_md5_ctx *context);
void app_md5_update(app_md5_ctx *context, uint8_t *buffer, uint32_t length);
void app_md5_final(uint8_t *result, app_md5_ctx *context);

#endif	// __md5_H
