/*
 * ephinea.dll — Deanchor pin
 * Original: FUN_52db2240 @ 0x52DB2240
 *
 * Installed at engine sites 0x4A9C0C / 0x4A9D28 / 0x5474D4.
 *
 * Forces 0xACC0E8/EC (engine UI design-anchor scale) to 1.0 around
 * specific engine 2D draw calls (effect/title quads at 0x82B158),
 * then restores the original values afterwards.
 *
 * This prevents certain full-screen effects and title screens from being
 * double-scaled by both the cascade and per-draw detour layers.
 */
void deanchor_pin(int* savedScale)
{
    float originalScale[2];
    
    /* Save current design-anchor scale */
    originalScale[0] = *(float*)0xACC0E8;
    originalScale[1] = *(float*)0xACC0EC;
    
    /* Pin to 1.0 (stock — no additional scaling) */
    *(float*)0xACC0E8 = _DAT_53175e64;  /* = 1.0f */
    *(float*)0xACC0EC = _DAT_53175e64;  /* = 1.0f */
    
    /* Call the original engine function at 0x82B158 */
    /* ... engine draw happens here ... */
    
    /* Restore */
    *(float*)0xACC0E8 = originalScale[0];
    *(float*)0xACC0EC = originalScale[1];
}