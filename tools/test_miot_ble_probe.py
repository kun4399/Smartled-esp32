import unittest
from types import SimpleNamespace

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

from tools.miot_ble_probe import (MiotBleProbe, get_property_packet, hkdf_sha256,
                                  pack_spec, parse_get_property, parse_set_property,
                                  set_bool_property_packet, set_uint_property_packet)


class MiotPacketTests(unittest.TestCase):
    def test_get_power_packet(self):
        self.assertEqual(get_property_packet(0x7201, 2, 1), bytes.fromhex("092001720201020100"))

    def test_set_power_on_packet(self):
        self.assertEqual(set_bool_property_packet(0x7202, 2, 1, True),
                         bytes.fromhex("0c2002720001020100010001"))

    def test_set_brightness_uint8_packet(self):
        self.assertEqual(set_uint_property_packet(0x7301, 2, 2, 75, 1),
                         bytes.fromhex("0c200173000102020001104b"))

    def test_set_color_temperature_uint16_packet(self):
        self.assertEqual(set_uint_property_packet(0x7302, 2, 3, 4000, 2),
                         bytes.fromhex("0d20027300010203000230a00f"))

    def test_uint_packet_rejects_overflow(self):
        with self.assertRaises(ValueError):
            set_uint_property_packet(1, 2, 2, 256, 1)

    def test_parse_set_property_response(self):
        packet = pack_spec(0x7202, 1, bytes.fromhex("010201000000"))
        self.assertEqual(parse_set_property(packet), {
            "tid": 0x7202, "opcode": 1,
            "properties": [{"siid": 2, "piid": 1, "code": 0}],
        })

    def test_parse_get_property_value(self):
        body = bytes.fromhex("010201000000011001")
        packet = pack_spec(0x7201, 3, body)
        parsed = parse_get_property(packet)
        self.assertEqual(parsed["tid"], 0x7201)
        self.assertEqual(parsed["properties"], [{"siid": 2, "piid": 1, "status": 0, "type": 1, "value": 1}])

    def test_parse_target_color_temperature_readback_shape(self):
        # Device returned type field 5 with a two-byte payload for 3574 K.
        packet = pack_spec(0x7203, 3, bytes.fromhex("0102030000000250f60d"))
        self.assertEqual(parse_get_property(packet)["properties"], [{
            "siid": 2, "piid": 3, "status": 0, "type": 5, "value": 3574,
        }])

    def test_rejects_truncated_response(self):
        with self.assertRaises(ValueError):
            parse_get_property(bytes.fromhex("08200172030102"))

    def test_hkdf_sha256_rfc5869_case_one(self):
        ikm = bytes.fromhex("0b" * 22)
        salt = bytes.fromhex("000102030405060708090a0b0c")
        info = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9")
        expected = bytes.fromhex(
            "3cb25f25faacd57a90434f64d0362f2a"
            "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
            "34007208d5b887185865"
        )
        self.assertEqual(hkdf_sha256(ikm, salt, info, 42), expected)

    def test_session_ccm_frame_round_trip(self):
        probe = MiotBleProbe(None, bytes(32))
        probe.key = bytes(range(64))
        packet = get_property_packet(0x7201, 2, 1)
        counter_bytes = (7).to_bytes(2, "little")
        tx_nonce = probe._nonce(probe.key[36:40], 7, 0)
        expected_tx = counter_bytes + AESCCM(probe.key[16:32], tag_length=4).encrypt(tx_nonce, packet, None)
        self.assertEqual(probe._encrypt(packet, 7), expected_tx)
        rx_nonce = probe._nonce(probe.key[32:36], 7, 0)
        rx_frame = counter_bytes + AESCCM(probe.key[:16], tag_length=4).encrypt(rx_nonce, packet, None)
        counter, decrypted = probe._decrypt(rx_frame)
        self.assertEqual(counter, 7)
        self.assertEqual(decrypted, packet)

    def test_target_advertisement_by_pid_when_name_is_missing(self):
        device = SimpleNamespace(name=None)
        advertisement = SimpleNamespace(
            local_name=None,
            service_data={"0000fe95-0000-1000-8000-00805f9b34fb":
                          b"\x00\x00\x15\x4d\x01"},
        )
        from tools.miot_ble_probe import is_target_advertisement
        self.assertTrue(is_target_advertisement(device, advertisement))


if __name__ == "__main__":
    unittest.main()
