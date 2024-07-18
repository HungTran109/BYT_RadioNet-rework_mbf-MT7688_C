#ifndef GSM_H
#define GSM_H

/**
 * \defgroup        gsm GSM
 * \brief           All gsm function process here
 * \
 */

#include <stdint.h>
#include <stdbool.h>

char *gsm_get_module_imei(void);
void gsm_set_serial_port(char *port);
uint8_t gsm_get_csq(void);
void gsm_disable_echo(void);
char *gsm_get_module_name(void);
bool gsm_send_at_cmd(int timeout_ms, int retires, char *expected_response, char *payload_response, const char *format, ...);
char *gsm_get_sim_imei_yoga(void);
char *gsm_get_sim_imei_ec2x(void);
char *gsm_get_sim_cimi(void);
void gsm_get_network_operator(char *nw_operator, char *technology);
void gsm_get_network_band_yoga_clm920(char *access_tech, char *band, char *channel);
void gsm_get_network_band_ec2x(char *access_tech, char *band, char *channel);
bool gsm_is_sim_inserted(void);
#endif // GSM_H
