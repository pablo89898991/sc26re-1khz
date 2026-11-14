# SC26RE_1k_SInput (eksperymentalny)

Osobny wariant firmware: po kablu USB kontroler przedstawia się jako urządzenie
**SInput** (protokół Hand Held Legend, obsługiwany natywnie przez sterownik SInput w SDL3)
zamiast Valve / Steam Controller HID.

## Co się zmienia (tylko USB)

| | Zwykły SC26RE_1k | SC26RE_1k_SInput |
|---|---|---|
| USB VID:PID | 28DE:1302 (Valve) | 2E8A:10C6 (generic SInput) |
| Nazwa USB | SC26RE-1kHz | SC26RE-1kHz SInput |
| Raport wejściowy | Valve 0x42, 1 kHz | SInput 0x01 (64 B), 1 kHz |
| Wibracje | Valve haptics (0x80…0x89) | SInput 0x03/0x01 (ERM typ 2 i freq/amp typ 1) → LRA w gripach |
| LED | sterowany przez Steam | kolor ustawiany komendą SInput „joystick RGB” |
| Lizard mode (mysz/klawiatura) | tak | brak po USB |
| Ustawienia Steam, wersja, aktualizacje | tak | brak po USB |

Puck (ESB) i Bluetooth działają **bez zmian** (protokół Valve).

## Mapowanie

- A/B/X/Y → South/East/West/North, D-pad, L3/R3, LB/RB
- LT/RT: analog (pełny zakres int16) + klik (dual-stage)
- L4/R4 → paddle 1, L5/R5 → paddle 2
- ☰ Menu → Start, ⧉ View → Back, Steam → Guide, QAM (…) → Share
- Klik lewego/prawego trackpada → Touchpad 1/2; oba trackpady jako 2 powierzchnie dotykowe z naciskiem
- Akcelerometr ±2 g, żyroskop ±2000 °/s, znacznik czasu IMU w µs
- Zgłaszany typ: Xbox One, układ przycisków ABXY, polling 1000 µs

## Uwaga

To wersja testowa. Gry/Steam muszą używać SDL3 z obsługą SInput (SDL 3.4+).
Aby wrócić do normalnego trybu, wgraj zwykły `SC26RE_1k.fw`.
