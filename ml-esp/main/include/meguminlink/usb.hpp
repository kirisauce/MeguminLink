#pragma once
#ifndef __MEGUMINLINK_USB_HPP__
#define __MEGUMINLINK_USB_HPP__

#include <esp_err.h>

namespace mlink
{
esp_err_t cmsis_dap_usb_init();
}

#endif