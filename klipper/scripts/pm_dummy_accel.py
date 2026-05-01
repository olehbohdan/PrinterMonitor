# Klipper extras plugin: pm_dummy_accel
#
# A no-op accelerometer that satisfies Klipper's [resonance_tester]
# accel_chip interface. It does not capture any samples; PrinterMonitor
# captures via USB independently. The sole purpose is to let the user
# run TEST_RESONANCES (which produces Klipper's deterministic MCU-timed
# chirp) without requiring a real adxl345/etc on the toolhead.
#
# Install: copy to ~/klipper/klippy/extras/pm_dummy_accel.py and add to
# printer.cfg:
#
#     [pm_dummy_accel pm_chip]
#
#     [resonance_tester]
#     accel_chip: pm_dummy_accel pm_chip
#     probe_points: 124, 115, 50    # bed center
#
# Then call e.g. TEST_RESONANCES AXIS=X OUTPUT=raw_data while the
# PrinterMonitor USB capture script runs in parallel.


class PmDummyAccelClient:
    """Dummy aclient: no samples, write_to_file emits a marker."""

    def __init__(self, name):
        self.name = name
        self._has_samples = False

    def finish_measurements(self):
        # Nothing to flush.
        pass

    def has_valid_samples(self):
        # If a caller asks, claim no samples so they fall through to
        # the helper-is-None path. resonance_tester only calls this
        # when OUTPUT=resonances, which we never use here.
        return False

    def write_to_file(self, path):
        try:
            with open(path, 'w') as f:
                f.write(
                    "# pm_dummy_accel: real samples captured via USB "
                    "by PrinterMonitor pm_usb_capture.py\n"
                )
        except Exception:
            # Best-effort; don't blow up the test on FS errors.
            pass

    def get_samples(self):
        return []

    def get_num_errors(self):
        return 0

    def get_msg_count(self):
        return 0


class PmDummyAccel:
    """Dummy accel chip: registers itself, hands out dummy clients."""

    def __init__(self, config):
        self.printer = config.get_printer()
        # Section name like "pm_dummy_accel pm_chip" -> name = "pm_chip".
        self.name = config.get_name().split()[-1]
        # axes_map kept for parity with adxl345 config syntax; ignored.
        config.get('axes_map', 'x,y,z')

    def start_internal_client(self):
        return PmDummyAccelClient(self.name)


def load_config_prefix(config):
    return PmDummyAccel(config)
