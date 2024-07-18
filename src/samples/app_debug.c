#include "app_debug.h"
#include <stdarg.h>
#include "stdio.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#if DEBUG_ISR_ENABLE
#include "lwrb.h"
#endif

#define ZEROPAD (1 << 0)   /* Pad with zero */
#define SIGN (1 << 1)      /* Unsigned/signed long */
#define UPPERCASE (1 << 6) /* 'ABCDEF' */
#define PLUS (1 << 2)      /* Show plus */
#define HEX_PREP (1 << 5)  /* 0x */
#define SPACE (1 << 3)     /* Spacer */
#define LEFT (1 << 4)      /* Left justified */
#define LIMIT (1 << 7)
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')

static uint8_t m_debug_level = DEBUG_LEVEL;
static app_debug_output_cb_t m_write_cb[DEBUG_NUMBER_OF_DEBUG_PORT];
static uint8_t number_of_callback = 0;
static app_debug_get_timestamp_ms_cb_t m_get_ms;
static app_debug_lock_cb_t m_lock_cb;
static const char *lower_digits = "0123456789abcdefghijklmnopqrstuvwxyz";
static const char *upper_digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static app_debug_fflush_cb_t m_flush_cb;

static uint32_t __strnlen(char *ptr, int32_t max_size)
{
    uint32_t len = 0;
    if (ptr == NULL || max_size == 0)
    {
        return 0;
    }
    if (max_size > 0)
    {
        while (*ptr++ && max_size--)
        {
            len++;
        }
    }
    else
    {
        while (*ptr++)
        {
            len++;
        }
    }
    return len;
}

#if DEBUG_ISR_ENABLE
static lwrb_t m_ringbuffer_debug_isr;
static uint8_t m_isr_buffer_size[DEBUG_ISR_RINGBUFFER_SIZE];
#endif

#ifdef APP_DEBUG_HAS_FLOAT

#include "math.h"

static int ilog10(double n) /* Calculate log10(n) in integer output */
{
    int rv = 0;

    while (n >= 10)
    { /* Decimate digit in right shift */
        if (n >= 100000)
        {
            n /= 100000;
            rv += 5;
        }
        else
        {
            n /= 10;
            rv++;
        }
    }
    while (n < 1)
    { /* Decimate digit in left shift */
        if (n < 0.00001)
        {
            n *= 100000;
            rv -= 5;
        }
        else
        {
            n *= 10;
            rv--;
        }
    }
    return rv;
}

static double i10x(int n) /* Calculate 10^n */
{
    double rv = 1;

    while (n > 0)
    { /* Left shift */
        if (n >= 5)
        {
            rv *= 100000;
            n -= 5;
        }
        else
        {
            rv *= 10;
            n--;
        }
    }
    while (n < 0)
    { /* Right shift */
        if (n <= -5)
        {
            rv /= 100000;
            n += 5;
        }
        else
        {
            rv /= 10;
            n++;
        }
    }
    return rv;
}

static void ftoa(
    char *buf,  /* Buffer to output the generated string */
    double val, /* Real number to output */
    int prec,   /* Number of fractinal digits */
    char fmt,    /* Notation */
    uint32_t max_size
)
{
    int d;
    int e = 0, m = 0;
    char sign = 0;
    double w;
    const char *er = 0;

    if (isnan(val))
    { /* Not a number? */
        er = "NaN";
    }
    else
    {
        if (prec < 0)
            prec = 6; /* Default precision (6 fractional digits) */
        if (val < 0)
        { /* Nagative value? */
            val = -val;
            sign = '-';
        }
        else
        {
            sign = '+';
        }
        if (isinf(val))
        { /* Infinite? */
            er = "INF";
        }
        else
        {
            if (fmt == 'f')
            {                           /* Decimal notation? */
                val += i10x(-prec) / 2; /* Round (nearest) */
                m = ilog10(val);
                if (m < 0)
                    m = 0;
                if (m + prec + 3 >= max_size)
                    er = "OV"; /* Buffer overflow? */
            }
            else
            { /* E notation */
                if (val != 0)
                {                                        /* Not a true zero? */
                    val += i10x(ilog10(val) - prec) / 2; /* Round (nearest) */
                    e = ilog10(val);
                    if (e > 99 || prec + 6 >= max_size)
                    { /* Buffer overflow or E > +99? */
                        er = "OV";
                    }
                    else
                    {
                        if (e < -99)
                            e = -99;
                        val /= i10x(e); /* Normalize */
                    }
                }
            }
        }
        if (!er)
        { /* Not error condition */
            if (sign == '-')
                *buf++ = sign; /* Add a - if negative value */
            do
            {                /* Put decimal number */
                w = i10x(m); /* Snip the highest digit d */
                d = val / w;
                val -= d * w;
                if (m == -1)
                    *buf++ = '.'; /* Insert a decimal separarot if get into fractional part */
                *buf++ = '0' + d;    /* Put the digit */
            } while (--m >= -prec);  /* Output all digits specified by prec */
            if (fmt != 'f')
            { /* Put exponent if needed */
                *buf++ = fmt;
                if (e < 0)
                {
                    e = -e;
                    *buf++ = '-';
                }
                else
                {
                    *buf++ = '+';
                }
                *buf++ = '0' + e / 10;
                *buf++ = '0' + e % 10;
            }
        }
    }
    if (er)
    { /* Error condition? */
        if (sign)
            *buf++ = sign; /* Add sign if needed */
        do
            *buf++ = *er++;
        while (*er); /* Put error symbol */
    }
    *buf = 0; /* Term */
}

#endif

void app_debug_init(app_debug_get_timestamp_ms_cb_t get_ms, app_debug_lock_cb_t lock_cb)
{
    m_get_ms = get_ms;
    m_lock_cb = lock_cb;
#if DEBUG_ISR_ENABLE
    lwrb_init(&m_ringbuffer_debug_isr, &m_isr_buffer_size, DEBUG_ISR_RINGBUFFER_SIZE);
#endif
}

uint32_t app_debug_get_ms(void)
{
    if (m_get_ms)
    {
        return m_get_ms();
    }
    return 0;
}

void app_debug_print_nothing(const char *fmt, ...)
{
    
}

void app_debug_putc(uint8_t data)
{
    for (uint8_t index = 0; index < DEBUG_NUMBER_OF_DEBUG_PORT; index++)
    {
        if (m_write_cb[index])
        {
            m_write_cb[index](&data, 1);
        }
    }
}

void put_string(char *s)
{
    if (s)
    {
        while (*s != '\0')
        {
            app_debug_putc((*s++));
        }
    }
    else
    {
        app_debug_putc('N');
        app_debug_putc('U');
        app_debug_putc('L');
        app_debug_putc('L');
    }
}

#if DEBUG_ISR_ENABLE

void app_debug_raw_isr(uint8_t c)
{
    lwrb_write(&m_ringbuffer_debug_isr, &c, 1);
}

static inline void put_byte_in_isr(uint8_t data)
{
    lwrb_write(&m_ringbuffer_debug_isr, &data, 1);
}

static inline void put_string_in_isr(char *s)
{
    if (s)
    {
        while (*s != '\0')
        {
            put_byte_in_isr((*s++));
        }
    }       // ignore null string in ISR
    // else
    // {
    //     char *null_ptr = "NULL";
    //     lwrb_write(&m_ringbuffer_debug_isr, null_ptr, 4);
    // }
}

static void custom_itoa_isr(int32_t val, int32_t radix, int32_t len)
{
    uint8_t c, r, sgn = 0, pad = ' ';
    uint8_t s[20], i = 0;
    uint32_t v;

    if (radix == 0)
    {
        return;
    }
    if (radix < 0)
    {
        radix = -radix;
        if (val < 0)
        {

            val = -val;
            sgn = '-';
        }
    }
    v = val;
    r = radix;
    if (len < 0)
    {
        len = -len;
        pad = '0';
    }

    if (len > 20)
    {
        return;
    }

    do
    {
        c = (uint8_t)(v % r);
        if (c >= 10)
            c += 7;
        c += '0';
        s[i++] = c;
        v /= r;
    } while (v);

    if (sgn)
    {
        s[i++] = sgn;
    }

    while (i < len)
    {
        s[i++] = pad;
    }

    do
    {
        put_byte_in_isr(s[--i]);
    } while (i);
}

#endif

#if 0
static void custom_itoa(int32_t val, int32_t radix, int32_t len)
{
    uint8_t c, r, sgn = 0, pad = ' ';
    uint8_t s[20], i = 0;
    uint32_t v;

    if (radix == 0)
    {
        return;
    }
    if (radix < 0)
    {
        radix = -radix;
        if (val < 0)
        {

            val = -val;
            sgn = '-';
        }
    }
    v = val;
    r = radix;
    if (len < 0)
    {
        len = -len;
        pad = '0';
    }

    if (len > 20)
    {
        return;
    }

    do
    {
        c = (uint8_t)(v % r);
        if (c >= 10)
            c += 7;
        c += '0';
        s[i++] = c;
        v /= r;
    } while (v);

    if (sgn)
    {
        s[i++] = sgn;
    }

    while (i < len)
    {
        s[i++] = pad;
    }

    do
    {
        app_debug_putc(s[--i]);
    } while (i);
}
#endif

// void app_debug_print_raw(const char *fmt, ...)
// {
//     if (m_lock_cb)
//     {
//         m_lock_cb(true, 0xFFFFFFFF);
//     }
// #if 0
//     int32_t n;
//     char *p = &m_debug_buffer[0];
//     int size = SEGGER_RTT_PRINTF_BUFFER_SIZE;
//     int time_stamp_size;

//     p += sprintf(m_debug_buffer, "<%u>: ", app_debug_get_ms());
//     time_stamp_size = (p-m_debug_buffer);
//     size -= time_stamp_size;
//     va_list args;

//     va_start (args, fmt);
//     n = vsnprintf(p, size, fmt, args);
//     if (n > (int)size)
//     {
//         for (uint8_t index = 0; index < DEBUG_NUMBER_OF_DEBUG_PORT; index++)
//         {
//             if (m_write_cb[index])
//             {
//                 m_write_cb[index](0, m_debug_buffer, n + time_stamp_size);
//             }
//         }

//     }
//     else if (n > 0)
//     {
//         for (uint8_t index = 0; index < DEBUG_NUMBER_OF_DEBUG_PORT; index++)
//         {
//             if (m_write_cb[index] != NULL)
//             {
//                 m_write_cb[index](0, m_debug_buffer, n + time_stamp_size);
//             }
//         }

//     }
//     va_end(args);
// #else

//     va_list arp;
//     int32_t d, r, w, s, l;
//     va_start(arp, fmt);

//     while ((d = *fmt++) != 0)
//     {
//         if (d != '%')
//         {
//             app_debug_putc(d);
//             continue;
//         }
//         const char *next = fmt;
//         if (*next == '%')
//         {
//             fmt++;
//             app_debug_putc('%');
//             continue;
//         }

//         d = *fmt++;
//         w = r = s = l = 0;

//         if (d == '0')
//         {
//             d = *fmt++; s = 1;
//         }

//         while ((d >= '0') && (d <= '9'))
//         {
//             w += w * 10 + (d - '0');
//             d = *fmt++;
//         }

//         if (s)
//         {
//             w = -w;
//         }

//         if (d == 'l')
//         {
//             l = 1;
//             d = *fmt++;
//         }

//         if (!d)
//         {
//             break;
//         }

//         if (d == 's')
//         {
//             put_string(va_arg(arp, char*));
//             continue;
//         }

//         if (d == 'c')
//         {
//             app_debug_putc((char)va_arg(arp, int32_t));
//             continue;
//         }

//         if (d == 'u') r = 10;
//         if (d == 'd') r = -10;
//         if (d == 'X' || d == 'x') r = 16; // 'x' added by mthomas in increase compatibility
//         if (d == 'b') r = 2;

//         if (!r)
//         {
//             break;
//         }

//         if (l)
//         {
//             custom_itoa((int32_t)va_arg(arp, int32_t), r, w);
//         }
//         else
//         {
//             if (r > 0)
//             {
//                 custom_itoa((uint32_t)va_arg(arp, int32_t), r, w);
//             }
//             else
//             {
//                 custom_itoa((int32_t)va_arg(arp, int32_t), r, w);
//             }
//         }
//     }
//     va_end(arp);
// #endif
//     if (m_lock_cb)
//     {
//         m_lock_cb(false, 0);
//     }
// }

static int ee_skip_atoi(const char **s)
{
    int i = 0;
    while (IS_DIGIT(**s))
        i = i * 10 + *((*s)++) - '0';
    return i;
}

static int ee_number(long num, int base, int size, int precision, int type)
{
    char c;
    char sign, tmp[66];
    const char *dig = lower_digits;
    int i;
    int number_of_byte = 0;

    if (type & UPPERCASE)
        dig = upper_digits;
    if (type & LEFT)
        type &= ~ZEROPAD;
    if (base < 2 || base > 36)
        return 0;

    c = (type & ZEROPAD) ? '0' : ' ';
    sign = 0;
    if (type & SIGN)
    {
        if (num < 0)
        {
            sign = '-';
            num = -num;
            size--;
        }
        else if (type & PLUS)
        {
            sign = '+';
            size--;
        }
        else if (type & SPACE)
        {
            sign = ' ';
            size--;
        }
    }

    if (type & HEX_PREP)
    {
        if (base == 16)
            size -= 2;
        else if (base == 8)
            size--;
    }

    i = 0;

    if (num == 0)
        tmp[i++] = '0';
    else
    {
        while (num != 0)
        {
            tmp[i++] = dig[((unsigned long)num) % (unsigned)base];
            num = ((unsigned long)num) / (unsigned)base;
        }
    }

    if (i > precision)
        precision = i;
    size -= precision;
    if (!(type & (ZEROPAD /* TINY option   | LEFT */)))
    {
        while (size-- > 0)
        {
            app_debug_putc(' ');
            number_of_byte++;
        }
    }
    if (sign)
    {
        app_debug_putc(sign);
        number_of_byte++;
    }

    if (type & HEX_PREP)
    {
        if (base == 8)
        {
            app_debug_putc('0');
            number_of_byte++;
        }
        else if (base == 16)
        {
            app_debug_putc('0');
            app_debug_putc(lower_digits[33]);
            number_of_byte += 2;
        }
    }

    if (!(type & LEFT))
    {
        while (size-- > 0)
        {
            app_debug_putc(c);
            number_of_byte++;
        }
    }
    while (i < precision--)
    {
        app_debug_putc('0');
        number_of_byte++;
    }
    while (i-- > 0)
    {
        app_debug_putc(tmp[i]);
        number_of_byte++;
    }
    while (size-- > 0)
    {
        app_debug_putc(' ');
        number_of_byte++;
    }

    return number_of_byte;
}

void app_debug_print_raw(uint8_t level, const char *fmt, ...)
{
    if (m_debug_level == DEBUG_LEVEL_NOTHING || level < m_debug_level)
    {
        return;
    }
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }
    unsigned long num;
    int base;
#ifdef APP_DEBUG_HAS_FLOAT
    char *str;
#endif
    int len;
    int i;
    char *s;

    int flags; // Flags to number()

    int field_width; // Width of output field
    int precision;   // Min. # of digits for integers; max number of chars for from string
    int qualifier;   // 'h', 'l', or 'L' for integer fields
    uint32_t nb_of_bytes = 0;
    va_list args;
    va_start(args, fmt);

    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            nb_of_bytes++;
            app_debug_putc(*fmt);
            continue;
        }

        // Process flags
        flags = 0;
    repeat:
        fmt++; // This also skips first '%'
        switch (*fmt)
        {
        case '-':
            flags |= LEFT;
            goto repeat;
        case '+':
            flags |= PLUS;
            goto repeat;
        case ' ':
            flags |= SPACE;
            goto repeat;
        case '#':
            flags |= HEX_PREP;
            goto repeat;
        case '0':
            flags |= ZEROPAD;
            goto repeat;
        }

        // Get field width
        field_width = -1;
        if (IS_DIGIT(*fmt))
            field_width = ee_skip_atoi(&fmt);
        else if (*fmt == '*')
        {
            fmt++;
            field_width = va_arg(args, int);
            if (field_width < 0)
            {
                field_width = -field_width;
                flags |= LEFT;
            }
        }

        // Get the precision
        precision = -1;
        if (*fmt == '.')
        {
            ++fmt;
            if (IS_DIGIT(*fmt))
                precision = ee_skip_atoi(&fmt);
            else if (*fmt == '*')
            {
                ++fmt;
                precision = va_arg(args, int);
            }
            if (precision < 0)
                precision = 0;
        }

        // Get the conversion qualifier
        qualifier = -1;
        if (*fmt == 'l' || *fmt == 'L')
        {
            qualifier = *fmt;
            fmt++;
        }

        // Default base
        base = 10;

        switch (*fmt)
        {
        case 'c':
            if (!(flags & LEFT))
            {
                while (--field_width > 0)
                {
                    app_debug_putc(' ');
                    nb_of_bytes++;
                };
            }

            app_debug_putc((unsigned char)va_arg(args, int));
            nb_of_bytes++;

            while (--field_width > 0)
            {
                app_debug_putc(' ');
                nb_of_bytes++;
            };
            continue;

        case 's':
            s = va_arg(args, char *);
            if (!s)
            {
                s = "<NULL>";
            }

            len = __strnlen(s, precision);
            if (!(flags & LEFT))
            {
                while (len < field_width--)
                {
                    app_debug_putc(' ');
                    nb_of_bytes++;
                };
            }
            for (i = 0; i < len; ++i)
            {
                app_debug_putc(*s++);
                nb_of_bytes++;
            };

            while (len < field_width--)
            {
                app_debug_putc(' ');
                nb_of_bytes++;
            };
            continue;

        case 'p':
            if (field_width == -1)
            {
                app_debug_putc('0');
                app_debug_putc('x');
                field_width = 2 * sizeof(void *);
                flags |= ZEROPAD;
            }
            nb_of_bytes += ee_number((unsigned long)va_arg(args, void *), 16, field_width, precision, flags);
            continue;

        case 'A':
            flags |= UPPERCASE;
            continue;

        // case 'a':
        //     if (qualifier == 'l')
        //         str = eaddr(str, va_arg(args, unsigned char *), field_width, precision, flags);
        //     else
        //         str = iaddr(str, va_arg(args, unsigned char *), field_width, precision, flags);
        //     continue;

        // Integer number formats - set up the flags and "break"
        case 'o':
            base = 8;
            break;

        case 'X':
            flags |= UPPERCASE;
            base = 16;
            break;

        case 'x':
            base = 16;
            break;

        case 'd':
        case 'i':
            flags |= SIGN;

        case 'u':
            break;

#ifdef APP_DEBUG_HAS_FLOAT

        case 'f':
        {
            char float_output[64];
            ftoa(float_output, va_arg(args, double), precision, *fmt, 63);
            for (uint32_t i = 0; i < 64 && float_output[i]; i++)
            {
                app_debug_putc(float_output[i]);
            }
        }
            continue;

#endif

        default:
            if (*fmt != '%')
            {
                app_debug_putc('%');
                nb_of_bytes++;
            }
            if (*fmt)
            {
                app_debug_putc(*fmt);
                nb_of_bytes++;
            }
            else
            {
                --fmt;
            }
            continue;
        }

        if (qualifier == 'l')
            num = va_arg(args, unsigned long);
        else if (flags & SIGN)
            num = va_arg(args, int);
        else
            num = va_arg(args, unsigned int);

        nb_of_bytes += ee_number(num, base, field_width, precision, flags);
    }
    va_end(args);

    if (m_flush_cb)
    {
        m_flush_cb(false);
    }

    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
    // return nb_of_bytes;
}

static void simple_print_hex(uint8_t hex_value)
{
    const char *hex_str = "0123456789ABCDEF";
    app_debug_putc(hex_str[(hex_value >> 4) & 0x0F]);
    app_debug_putc(hex_str[(hex_value)&0x0F]);
}

#if DEBUG_ISR_ENABLE
void app_debug_print_isr(const char *fmt, ...)
{
    if (m_debug_level == DEBUG_LEVEL_NOTHING)
    {
        return;
    }
#if 0
    int32_t n;
    char *p = &m_debug_buffer[0];
    int size = SEGGER_RTT_PRINTF_BUFFER_SIZE;
    int time_stamp_size;

    p += sprintf(m_debug_buffer, "<%u>: ", app_debug_get_ms());
    time_stamp_size = (p-m_debug_buffer);
    size -= time_stamp_size;
    va_list args;

    va_start (args, fmt);
    n = vsnprintf(p, size, fmt, args);
    if (n > (int)size) 
    {
        for (uint8_t index = 0; index < DEBUG_NUMBER_OF_DEBUG_PORT; index++)
        {
            if (m_write_cb[index])
            {
                m_write_cb[index](0, m_debug_buffer, n + time_stamp_size);
            }
        }
        
    } 
    else if (n > 0) 
    {
        for (uint8_t index = 0; index < DEBUG_NUMBER_OF_DEBUG_PORT; index++)
        {
            if (m_write_cb[index] != NULL)
            {
                m_write_cb[index](0, m_debug_buffer, n + time_stamp_size);
            }
        }

    }
    va_end(args);
#else

    va_list arp;
    int32_t d, r, w, s, l;
    va_start(arp, fmt);

    while ((d = *fmt++) != 0)
    {
        if (d != '%')
        {
            put_byte_in_isr(d);
            continue;
        }
        const char *next = fmt;
        if (*next == '%')
        {
            fmt++;
            put_byte_in_isr('%');
            continue;
        }

        d = *fmt++;
        w = r = s = l = 0;

        if (d == '0')
        {
            d = *fmt++;
            s = 1;
        }

        while ((d >= '0') && (d <= '9'))
        {
            w += w * 10 + (d - '0');
            d = *fmt++;
        }

        if (s)
        {
            w = -w;
        }

        if (d == 'l')
        {
            l = 1;
            d = *fmt++;
        }

        if (!d)
        {
            break;
        }

        if (d == 's')
        {
            put_string_in_isr(va_arg(arp, char *));
            continue;
        }

        if (d == 'c')
        {
            put_byte_in_isr((char)va_arg(arp, int32_t));
            continue;
        }

        if (d == 'u')
            r = 10;
        if (d == 'd')
            r = -10;
        if (d == 'X' || d == 'x')
            r = 16; // 'x' added by mthomas in increase compatibility
        if (d == 'b')
            r = 2;

        if (!r)
        {
            break;
        }

        if (l)
        {
            custom_itoa_isr((int32_t)va_arg(arp, int32_t), r, w);
        }
        else
        {
            if (r > 0)
            {
                custom_itoa_isr((uint32_t)va_arg(arp, int32_t), r, w);
            }
            else
            {
                custom_itoa_isr((int32_t)va_arg(arp, int32_t), r, w);
            }
        }
    }
    va_end(arp);
#endif
}

/**
 * @brief           Flush all data in ringbuffer of ISR
 */
void app_debug_isr_ringbuffer_flush(void)
{
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }

    uint8_t tmp;
    while (lwrb_read(&m_ringbuffer_debug_isr, &tmp, 1))
    {
        app_debug_putc(tmp);
    }

    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}

#else

void app_debug_print_isr(const char *fmt, ...)
{
    
}

void app_debug_isr_ringbuffer_flush(void)
{

}

#endif /* DEBUG_ISR_ENABLE */

void app_debug_dump(const void *data, int32_t len, const char *message)
{
    if (m_debug_level == DEBUG_LEVEL_NOTHING)
    {
        return;
    }
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }

    uint8_t *p = (uint8_t *)data;
    uint8_t buffer[16];
    int32_t i_len;
    int32_t i;

    char tmp[32];
    sprintf(tmp, "%u", (uint32_t)len);
    put_string((char *)message);
    put_string(" : ");
    put_string(tmp);
    put_string(" bytes\r\n");

    //    DEBUG_RAW("%s : %u bytes\r\n", message, len);

    while (len > 0)
    {
        i_len = (len > 16) ? 16 : len;
        memset(buffer, 0, 16);
        memcpy(buffer, p, i_len);
        for (i = 0; i < 16; i++)
        {
            if (i < i_len)
            {
                simple_print_hex(buffer[i]);
                app_debug_putc(' ');
            }
            else
            {
                put_string("   ");
            }
        }
        put_string("\t");
        for (i = 0; i < 16; i++)
        {
            if (i < i_len)
            {
                if (isprint(buffer[i]))
                {
                    sprintf(tmp, "%c", (char)buffer[i]);
                    put_string(tmp);
                }
                else
                {
                    put_string(".");
                }
            }
            else
            {
                put_string(" ");
            }
        }
        put_string("\r\n");
        len -= i_len;
        p += i_len;
    }
    
    if (m_flush_cb)
    {
        m_flush_cb(false);
    }

    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}

void app_debug_register_callback_print(app_debug_output_cb_t callback)
{
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }
    uint8_t callback_exist = 0; // Check for existion function pointer in function pointer arry
    if (callback)
    {
        for (uint8_t func_count = 0; func_count < DEBUG_NUMBER_OF_DEBUG_PORT; func_count++)
        {
            if (callback == m_write_cb[func_count])
            {
                // Callback already existed in array
                callback_exist = 1;
            }
        }

        if (!callback_exist)
        {
            m_write_cb[number_of_callback] = callback;
            number_of_callback++;
        }
    }
    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}

void app_debug_unregister_callback_print(app_debug_output_cb_t callback)
{
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }
    for (uint8_t func_count = 0; func_count < DEBUG_NUMBER_OF_DEBUG_PORT; func_count++)
    {
        if (callback && callback == m_write_cb[func_count])
        {
            m_write_cb[func_count] = NULL;
            number_of_callback--;
        }
    }
    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}

void app_debug_register_fflush_cb(app_debug_fflush_cb_t flush_cb)
{
    m_flush_cb = flush_cb;
}

void app_debug_disable(void)
{
    m_debug_level = DEBUG_LEVEL_NOTHING;
}

void app_debug_set_level(uint8_t level)
{
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }
    m_debug_level = level;
    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}

void app_debug_flush()
{
    if (m_lock_cb)
    {
        m_lock_cb(true, 0xFFFFFFFF);
    }
    if (m_flush_cb)
    {
        m_flush_cb(false);
    }
    if (m_lock_cb)
    {
        m_lock_cb(false, 0);
    }
}
