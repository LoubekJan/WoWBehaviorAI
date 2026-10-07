"""Admission diagnostics are optional for legacy payloads and independent of HTTP."""
import unittest

from pydantic import ValidationError
from app.telemetry import AdviceAdmission, RecoveryAdvice


def legacy():
    return dict(lifetime_ms=123, enabled=True, pending=False, status='NO_VALID_OPTIONS',
                requests=0, selected=0, started=0, arrived=0, home_success=0,
                food_success=0, rejected=0, unavailable=0, reused=0)


def evidence():
    return dict(ready_observed=3, entries=2, acquire_attempts=1, acquired=1,
                early_gates=1, local_empty_options=1, last_event='NO_VALID_OPTIONS',
                last_reason='NO_VALID_OPTIONS', last_event_age_ms=5, last_acquired_age_ms=40)


class AdviceAdmissionTests(unittest.TestCase):
    def test_old_snapshots_have_unavailable_admission(self):
        old = RecoveryAdvice.model_validate(legacy())
        self.assertIsNone(old.admission)
        self.assertIsNone(RecoveryAdvice.model_validate({**legacy(), 'admission': None}).admission)
        self.assertIsNone(old.model_dump()['admission'])

    def test_new_owned_values_roundtrip_without_inventing_http(self):
        source = evidence()
        parsed = RecoveryAdvice.model_validate({**legacy(), 'admission': source})
        source['entries'] = 100
        source['last_reason'] = 'CHANGED'
        self.assertEqual(parsed.admission.entries, 2)
        self.assertEqual(parsed.admission.last_reason, 'NO_VALID_OPTIONS')
        again = RecoveryAdvice.model_validate_json(parsed.model_dump_json())
        self.assertEqual(parsed, again)
        self.assertEqual(parsed.requests, 0)
        self.assertFalse(parsed.pending)

    def test_uint32_counts_and_uint64_nullable_ages_are_strict(self):
        for field in ('ready_observed', 'entries', 'acquire_attempts', 'acquired', 'early_gates', 'local_empty_options'):
            self.assertEqual(getattr(AdviceAdmission.model_validate({**evidence(), field: 4294967295}), field), 4294967295)
            for bad in (-1, 4294967296, True, '1', 1.5, None):
                with self.subTest(field=field, bad=bad), self.assertRaises(ValidationError):
                    AdviceAdmission.model_validate({**evidence(), field: bad})
        for field in ('last_event_age_ms', 'last_acquired_age_ms'):
            self.assertIsNone(getattr(AdviceAdmission.model_validate({**evidence(), field: None}), field))
            self.assertEqual(getattr(AdviceAdmission.model_validate({**evidence(), field: 18446744073709551615}), field), 18446744073709551615)
            for bad in (-1, 18446744073709551616, True, '1'):
                with self.subTest(field=field, bad=bad), self.assertRaises(ValidationError):
                    AdviceAdmission.model_validate({**evidence(), field: bad})

    def test_reason_event_and_unknown_keys_remain_bounded(self):
        for field, bad in (('last_reason', 'x' * 81), ('last_reason', None),
                           ('last_event', 'HTTP_SUCCESS'), ('unknown', 1)):
            with self.subTest(field=field), self.assertRaises(ValidationError):
                AdviceAdmission.model_validate({**evidence(), field: bad})


if __name__ == '__main__':
    unittest.main()
