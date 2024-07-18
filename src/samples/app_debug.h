#ifndef APP_DEBUG_H
#define APP_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

#define DEBUG_LEVEL_ALL                 0
#define DEBUG_LEVEL_VERBOSE             1
#define DEBUG_LEVEL_INFO                2
#define DEBUG_LEVEL_WARN                3
#define DEBUG_LEVEL_ERROR               4
#define DEBUG_LEVEL_NOTHING             5
#define DEBUG_LEVEL                     DEBUG_LEVEL_VERBOSE  

#ifndef DEBUG_ISR_ENABLE
#define DEBUG_ISR_ENABLE                0       // allow debug inside interrupt service
#define DEBUG_ISR_RING_BUFFER_SIZE      256
#endif

#ifndef DEBUG_NUMBER_OF_DEBUG_PORT
#define DEBUG_NUMBER_OF_DEBUG_PORT 2
#endif

// Uncomment to disable float print
#ifndef APP_DEBUG_HAS_FLOAT
//#define APP_DEBUG_HAS_FLOAT
#endif

#if 0
    #define KNRM  "\x1B[0m"
    #define KRED  RTT_CTRL_TEXT_RED
    #define KGRN  RTT_CTRL_TEXT_GREEN
    #define KYEL  RTT_CTRL_TEXT_YELLOW
    #define KBLU  RTT_CTRL_TEXT_BLUE
    #define KMAG  RTT_CTRL_TEXT_MAGENTA
    #define KCYN  RTT_CTRL_TEXT_CYAN
    #define KWHT  RTT_CTRL_TEXT_WHITE
#else
    #define KNRM  "\x1B[0m"
    #define KRED  "\x1B[31m"
    #define KGRN  "\x1B[32m"
    #define KYEL  "\x1B[33m"
    #define KBLU  "\x1B[34m"
    #define KMAG  "\x1B[35m"
    #define KCYN  "\x1B[36m"
    #define KWHT  "\x1B[37m"
#endif

#define DEBUG_RAW(args...)                      app_debug_print_raw(DEBUG_LEVEL_ERROR, args)
#define DEBUG_DUMP                              app_debug_dump
#define DEBUG_FLUSH                             app_debug_flush

#if DEBUG_ISR_ENABLE
    #define DEBUG_ISR(s, args...)               app_debug_print_isr(KBLU "[ISR] %s : " s KNRM, "", ##args)
#else
    #define DEBUG_ISR(s, args...)                            
#endif

#if (DEBUG_LEVEL_VERBOSE >= DEBUG_LEVEL)
#define DEBUG_VERBOSE(s, args...)               app_debug_print_raw(DEBUG_LEVEL_VERBOSE, KMAG "<%u> %s : " s KNRM,  app_debug_get_ms(), "", ##args)
#else
#define DEBUG_VERBOSE(s, args...)               // app_debug_print_nothing(s, ##args)
#endif

#if (DEBUG_LEVEL_INFO >= DEBUG_LEVEL)
#define DEBUG_INFO(s, args...)                  app_debug_print_raw(DEBUG_LEVEL_INFO, KGRN "<%u> %s : " s KNRM,  app_debug_get_ms(), "", ##args)
#else
#define DEBUG_INFO(s, args...)                  // app_debug_print_nothing(s, ##args)
#endif

#if (DEBUG_LEVEL_ERROR >= DEBUG_LEVEL)
#define DEBUG_ERROR(s, args...)                 app_debug_print_raw(DEBUG_LEVEL_ERROR, KRED "<%u> %s : " s KNRM,  app_debug_get_ms(), "", ##args)
#else
#define DEBUG_ERROR(s, args...)                 // app_debug_print_nothing(s, ##args)
#endif

#if (DEBUG_LEVEL_WARN >= DEBUG_LEVEL)
#define DEBUG_WARN(s, args...)                  app_debug_print_raw(DEBUG_LEVEL_WARN, KYEL "<%u> %s : " s KNRM,  app_debug_get_ms(), "", ##args)
#else
#define DEBUG_WARN(s, args...)                  // app_debug_print_nothing(s, ##args)
#endif

#define DEBUG_COLOR(color, s, args...)          app_debug_print_raw(color s KNRM, ##args)


typedef uint32_t (*app_debug_get_timestamp_ms_cb_t)(void);      // Get timestamp data 
typedef uint32_t (*app_debug_output_cb_t)(const void *buffer, uint32_t len);
typedef bool (*app_debug_lock_cb_t)(bool lock, uint32_t timeout_ms);
typedef void (*app_debug_fflush_cb_t)(bool lock);

/**
 * @brief           Initialize debug module
 * @param[in]       get_ms System get tick in ms
 */
void app_debug_init(app_debug_get_timestamp_ms_cb_t get_ms, app_debug_lock_cb_t lock_cb);

/**
 * @brief           Add call back function to debug array
 *                  Add your function to print data on screen, such as UartTransmit, CDCTransmit,...
 * @param[in]       output_cb Output data callback
 */
void app_debug_register_callback_print(app_debug_output_cb_t output_cb);

/**
 * @brief           Remove callback function from debug callback array
 * @param[in]       output_cb  Function which do you want to remove
 */
void app_debug_unregister_callback_print(app_debug_output_cb_t output_cb);

/**
 * @brief           Register flush callback
 */
void app_debug_register_fflush_cb(app_debug_fflush_cb_t flush_cb);

/**
 * @brief           Get timebase value in milliseconds
 * @retval          Current counter value of the timebase 
 */
uint32_t app_debug_get_ms(void);

/**
 * @brief           Print nothing on screen
 *                  Called when the DEBUG_FUNCTION level is less than the DEBUG_LEVEL value 
 */
void app_debug_print_nothing(const char *fmt, ...);

/**
 * @brief           Print data on screen, link standard C printf
 * @param[in]       fmt Pointer to the format want to print on the screen
 * @retval          Number of bytes was printed, -1 on error
 */
int32_t app_debug_print(const char *fmt, ...);

/**
 * @brief           Print data on screen, link standard C printf inside isr service
 * @param[in]       fmt Pointer to the format want to print on the screen
 * @retval          Number of bytes was printed, -1 on error
 */
void app_debug_print_isr(const char *fmt, ...);

/**
 * @brief           Flush all data in ringbuffer of ISR
 */
void app_debug_isr_ringbuffer_flush(void);


/**
 * @brief           Print raw data
 * @param[in]       level debug level
 * @param[in]       fmt Pointer to the data want to print on the screen      
 */
void app_debug_print_raw(uint8_t level, const char *fmt, ...);

/**
 * @brief           Dump hexa data to screen
 * @param[in]       data Pointer to the data want to print on the screen
 * @param[in]       len Number of bytes want to print
 * @param[in]       message Debug message
 * @retval          Number of bytes was printed, -1 on error    
 */
void app_debug_dump(const void* data, int32_t len, const char* message);

/**
 * @brief           Put byte to debug port
 * @param[in]       c Data to send
 */
void app_debug_putc(uint8_t c);

/**
 * @brief           Put byte to debug port in ISR context
 * @param[in]       c Data to send
 */
void app_debug_raw_isr(uint8_t c);

/**
 * @brief           Disable debug log output
 */
void app_debug_disable(void);

/**
 * @brief           Flush serial
 */
void app_debug_flush(void);

/**
 * @brief           Set debug level
 * @param[in]       level Debug level
 */
void app_debug_set_level(uint8_t level);

#endif // !APP_DEBUG_H
