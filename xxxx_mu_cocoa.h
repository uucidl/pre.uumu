enum {
  MU_CMD = 0x37,    // kVK_Command
  MU_SHIFT = 0x38,  // kVK_Shift
  MU_OPTION = 0x3A, // kVK_Option
  MU_CTRL = 0x3B,   // kVK_Control
};

// forward declaration
#if !defined(__OBJC__)
typedef struct NSWindow NSWindow;
#else
@class NSWindow;
#endif

struct Mu_Cocoa {
  NSWindow *window;
};

