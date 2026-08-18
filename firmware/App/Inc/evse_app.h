/**
 * @file  evse_app.h
 * @brief Application entry point — call this from main() and never return.
 *
 * Integration with a CubeMX project is two lines:
 *
 *     MX_GPIO_Init(); MX_ADC1_Init(); ... MX_ICACHE_Init();
 *     evse_app_start();          // creates the tasks and starts the scheduler
 *
 * Everything below this is owned by the application; the generated code is left
 * untouched so the .ioc can be regenerated freely.
 */
#ifndef EVSE_APP_H
#define EVSE_APP_H

/** Never returns. */
void evse_app_start(void);

#endif /* EVSE_APP_H */
