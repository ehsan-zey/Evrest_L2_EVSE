# Metering and over-current protection

The metering front end is an **ATM90E26** on UART4 (`ENME_TX` PD12 /
`ENME_RX` PD11) at 9600 8N1, with its IRQ output on PE8 (`ENME_IRQ`, EXTI8
falling).

## What the IRQ line can actually do

The intent described for this board was that the ATM90E26 asserts IRQ on
over-voltage or on over-current beyond the charger limit, and the MCU opens the
relay. **The ATM90E26 cannot do the over-current half of that.**

Its interrupt sources, enabled through `FuncEn` (0x02) and reported in
`SysStatus` (0x01), are:

| Source | Register | What it means |
|---|---|---|
| Voltage **sag** | threshold in `SagTh` (0x03) | Urms fell below a programmed level |
| Reverse **active** energy | `RevPchg` | active power changed direction |
| Reverse **reactive** energy | `RevQchg` | reactive power changed direction |

There is no register that asserts IRQ when Irms exceeds a programmed value.
`PStartTh` (0x0F) and `PNolTh` (0x10) look like candidates but are startup and
no-load *power* thresholds that gate energy accumulation — they do not drive the
IRQ pin. Likewise there is no over-*voltage* interrupt; `SagTh` is one-sided and
only fires on voltage falling.

Relying on the IRQ alone for over-current would therefore leave the unit with no
over-current protection at all, and the failure would be silent — the IRQ line
would simply never assert.

## What the firmware does instead

Both mechanisms are used, for the things each is actually good at:

**IRQ — voltage sag.** `SagTh` is programmed from `METER_SAG_THRESHOLD_V`
(180 V default). `meter_irq_isr()` records the event, `meter_update()` reads
`SysStatus` to confirm which source fired, and `safety_update()` raises
`EVSE_FAULT_UNDER_VOLTAGE`, which is in the trip mask and opens the contactor.

The ISR deliberately does *not* open the contactor directly, unlike the RCD
interrupt. Sag and reverse-energy share one IRQ pin, and reverse-energy is not a
trip condition — dropping the contactor before reading `SysStatus` would turn a
harmless direction change into a lost session.

**Polling — over-current.** `safety_update()` runs every 5 ms and compares the
measured current against the current the pilot is advertising:

```
> 130 % of the offer, sustained 100 ms   -> trip
> 110 % of the offer, sustained 5000 ms  -> trip
```

Two thresholds so a vehicle's inrush does not nuisance-trip while a genuine
overdraw is still caught quickly. Below `OC_MIN_MEANINGFUL_A` (2 A) the ratio is
dominated by measurement noise and the test is skipped.

The 5 ms cadence is why the safety task is the highest priority in the system
and why it never blocks on the metering UART — `meter_last_current_a()` returns
a plain cached float rather than taking the UART mutex.

**Over-voltage** is handled the same way, as a sustained comparison against
`VOLTAGE_MAX_V` over `VOLTAGE_FAULT_MS`, since the IC offers no interrupt for it.

## If you want a hardware over-current trip

Nothing in the firmware precludes it, and for a Level-2 unit it is worth having.
Two options that fit this board:

1. **Comparator on the CT burden.** Feed the metering CT's burden voltage to one
   of the H5's on-chip comparators (COMP1/COMP2) with the threshold set from the
   DAC, and route its output to `TIM1`'s break input. A break event drops the
   pilot in hardware, with no code in the path. Free pins are listed in
   `evse_board.h`.
2. **A second CT into an external comparator** driving the relay coil supply
   directly, which is what most certified units do because it survives an MCU
   lockup.

Until one of those exists, the 5 ms poll is the protection, and its worst-case
response — 5 ms detection + 100 ms confirmation + 20 ms contactor drop-out — is
what should be quoted in the design file.

## Calibration

`ATM_CAL_*` in `meter.c` are the datasheet reference-design values. They will
read within a few percent, which is fine for a dashboard and not fine for
billing. A production calibration against a reference source should write
`Ugain` (0x21), `IgainL` (0x22), `Uoffset` (0x24) and `IoffsetL` (0x25), and the
resulting values belong in NVM rather than in the source.

Energy is integrated in software from `Pmean` rather than read from `APenergy`,
because the energy register is read-to-clear: a dropped or retried UART frame
loses that energy permanently and invisibly. Integrating at 250 ms costs at most
one sample on a failed read, and the failure shows up in the fault counter.
