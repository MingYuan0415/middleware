#ifndef __FILE_SERVICE_FAKE_ESP_LOG_H__
#define __FILE_SERVICE_FAKE_ESP_LOG_H__

/* Host tests keep service logging out of the test output. */
#define ESP_LOGE(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)(tag), (void)0)

#endif /* __FILE_SERVICE_FAKE_ESP_LOG_H__ */
