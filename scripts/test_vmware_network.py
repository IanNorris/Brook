#!/usr/bin/env python3
"""Verify VMware network defaults without building or booting an image."""

import argparse
import unittest
from unittest.mock import patch

import brook_image
import make_vmware_image


REQUIRED = ['--output', 'vm', '--operator-policy', 'operators.pub',
            '--recovery-checkers-from', 'recovery.img.gz']


class VmwareNetworkTest(unittest.TestCase):
    def check_vmx_network(self, options, expected):
        parsed = make_vmware_image.argument_parser().parse_args(options)
        self.assertEqual(parsed.network, expected)
        connection = [line for line in make_vmware_image.vmx(parsed.network).splitlines()
                      if line.startswith('ethernet0.connectionType = ')]
        self.assertEqual(connection, [f'ethernet0.connectionType = "{expected}"'])

    def test_builder_defaults_to_nat(self):
        self.check_vmx_network(REQUIRED, 'nat')

    def test_builder_preserves_explicit_host_only(self):
        self.check_vmx_network([*REQUIRED, '--network', 'hostonly'], 'hostonly')

    def test_image_command_inherits_default_and_forwards_override(self):
        for apps in (None, ['firefox']):
            for network, expected in ((None, 'nat'), ('hostonly', 'hostonly')):
                with self.subTest(apps=apps, network=network):
                    args = argparse.Namespace(output='vm', operator_policy='operators.pub',
                                              recovery_checkers_from='recovery.img.gz', network=network,
                                              payload_id=None, data_mib=None, preset=None, apps=apps,
                                              packages=[], config={'target': 'vmware', 'apps': apps})
                    with patch.object(brook_image, 'script') as invoke:
                        brook_image.build_vmware(args)
                    invoke.assert_called_once()
                    script, *options = invoke.call_args.args
                    self.assertEqual(script, 'make_vmware_image.py')
                    self.assertEqual('--network' in options, network is not None)
                    self.check_vmx_network(list(map(str, options)), expected)


if __name__ == '__main__':
    unittest.main()
