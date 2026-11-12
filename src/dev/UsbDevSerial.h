#pragma once

#if defined(CROSSPOINT_USB_DEV) && CROSSPOINT_USB_DEV

class GfxRenderer;

class UsbDevSerial {
 public:
  static void tickInput();
  static bool pollCommands(GfxRenderer& renderer);
  static bool skipLoopDelay();
};

#endif
