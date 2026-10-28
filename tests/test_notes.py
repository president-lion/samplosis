def test_version(lib):
    assert lib.smp_version() == b"0.1.0"
