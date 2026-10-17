// Lion pad handling for ULionViewport (included by LionDrv.cpp).
#define PAD_REG(a) (*(volatile DWORD*)(a))
enum { kPadBase = 0x1F050000 };

UBOOL ULionViewport::PadEvent(INT iKey, UBOOL Down) {
  if (Down == (UBOOL)Input->KeyDown(iKey)) return 1;
  return Client->Engine->InputEvent(this, (EInputKey)iKey, Down ? IST_Press : IST_Release);
}

void ULionViewport::UpdateInput(UBOOL) {
  // Digital buttons: D-pad walks/turns, A fires, B jumps, C alt-fire, X use item, L/R switch weapon.
  static const struct { DWORD Mask; INT Key; } Map[] = {
    {1u << 0, IK_Up}, {1u << 1, IK_Down}, {1u << 2, IK_Left}, {1u << 3, IK_Right},
    {1u << 4, IK_Joy1}, {1u << 5, IK_Joy2}, {1u << 6, IK_Joy3}, {1u << 7, IK_Joy12},
    {1u << 8, IK_Joy4}, {1u << 9, IK_Joy5}, {1u << 10, IK_Joy11}, {1u << 11, IK_Joy13},
    {1u << 12, IK_Escape}, {1u << 13, IK_Tab}};
  const DWORD Buttons = PAD_REG(kPadBase + 0);
  for (INT i = 0; i < (INT)(sizeof(Map) / sizeof(Map[0])); ++i) {
    const UBOOL Down = (Buttons & Map[i].Mask) != 0;
    if (Down != ((LastButtons & Map[i].Mask) != 0)) PadEvent(Map[i].Key, Down);
  }
  LastButtons = Buttons;

  // Analog sticks: left = move/strafe, right = turn/look. Same dead zone as the Windows driver.
  const DWORD Sticks = PAD_REG(kPadBase + 4);
  const INT Raw[4] = {(INT)(Sticks >> 24) & 255, (INT)(Sticks >> 16) & 255, (INT)(Sticks >> 8) & 255, (INT)Sticks & 255};
  static const struct { INT Key; FLOAT Scale; } Axes[4] = {
    {IK_JoyX, 1000.0f}, {IK_JoyY, -1000.0f}, {IK_JoyU, 2000.0f}, {IK_JoyV, -2000.0f}};
  for (INT i = 0; i < 4; ++i) {
    FLOAT D = (Raw[i] - 128) / 128.0f;
    if (D > 0.2f) D = (D - 0.2f) / 0.8f;
    else if (D < -0.2f) D = (D + 0.2f) / 0.8f;
    else D = 0;
    if (D != 0 || LastStick[i] != 0) Client->Engine->InputEvent(this, (EInputKey)Axes[i].Key, IST_Axis, Axes[i].Scale * D);
    LastStick[i] = D;
  }
}
