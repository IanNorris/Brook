#!/usr/bin/env python3
"""Verify VMware VMX configuration without building or booting an image."""

import argparse
import unittest
from unittest.mock import patch

import brook_image
import make_vmware_image
import make_vmware_probe


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

    def test_generated_vmx_enables_hd_audio(self):
        vmx = make_vmware_image.vmx('nat').splitlines()
        self.assertEqual([line for line in vmx if line.startswith('sound.present')],
                         ['sound.present = "TRUE"'])
        self.assertIn('sound.virtualDev = "hdaudio"', vmx)
        self.assertIn('sound.startConnected = "TRUE"', vmx)

    def test_generated_vmx_disables_floppy(self):
        for vmx in (make_vmware_image.vmx('nat'), make_vmware_probe.PROBE_VMX):
            self.assertEqual([line for line in vmx.splitlines() if line.startswith('floppy0.')],
                             ['floppy0.present = "FALSE"'])

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


def vmx_values(text):
    return dict(line.split(' = ', 1) for line in text.splitlines())


class VmwareResourceTest(unittest.TestCase):
    def test_builder_defaults_to_four_cpus_and_eight_gib(self):
        parsed = make_vmware_image.argument_parser().parse_args(REQUIRED)
        self.assertEqual((parsed.cpus, parsed.memory_mib), (4, 8192))
        values = vmx_values(make_vmware_image.vmx(parsed.network, parsed.cpus, parsed.memory_mib))
        self.assertEqual(values['numvcpus'], '"4"')
        self.assertEqual(values['cpuid.coresPerSocket'], '"4"')
        self.assertEqual(values['memsize'], '"8192"')
        self.assertEqual(values['floppy0.present'], '"FALSE"')
        self.assertEqual(values['ethernet0.connectionType'], '"nat"')
        self.assertEqual(values['sound.virtualDev'], '"hdaudio"')
        self.assertEqual(vmx_values(make_vmware_image.vmx('nat')), values)

    def test_builder_overrides_resources(self):
        parsed = make_vmware_image.argument_parser().parse_args(
            [*REQUIRED, '--cpus', '12', '--memory-mib', '16384'])
        make_vmware_image.check_resources(make_vmware_image.argument_parser(), parsed)
        values = vmx_values(make_vmware_image.vmx(parsed.network, parsed.cpus, parsed.memory_mib))
        self.assertEqual(values['numvcpus'], '"12"')
        self.assertEqual(values['cpuid.coresPerSocket'], '"6"')
        self.assertEqual(values['memsize'], '"16384"')
        self.assertEqual(make_vmware_image.cores_per_socket(2), 2)
        self.assertEqual(make_vmware_image.cores_per_socket(13), 1)

    def test_builder_rejects_invalid_resources(self):
        for options in (['--cpus', '0'], ['--cpus', '33'], ['--memory-mib', '512'],
                        ['--memory-mib', '8191']):
            with self.subTest(options=options):
                parser = make_vmware_image.argument_parser()
                parsed = parser.parse_args([*REQUIRED, *options])
                with patch('sys.stderr'), self.assertRaises(SystemExit):
                    make_vmware_image.check_resources(parser, parsed)

    def test_image_command_forwards_resources(self):
        args = argparse.Namespace(output='vm', operator_policy='operators.pub',
                                  recovery_checkers_from='recovery.img.gz', network=None,
                                  payload_id=None, data_mib=None, apps=None, packages=[],
                                  cpus=2, memory_mib=4096)
        with patch.object(brook_image, 'script') as invoke:
            brook_image.build_vmware(args)
        options = list(map(str, invoke.call_args.args[1:]))
        parsed = make_vmware_image.argument_parser().parse_args(options)
        self.assertEqual((parsed.cpus, parsed.memory_mib), (2, 4096))

    def test_default_apps_drop_firefox_and_add_calculators(self):
        apps = make_vmware_image.DEFAULT_APPS
        self.assertNotIn('firefox', apps)
        self.assertIn('calculator', apps)
        self.assertIn('qalculate', apps)


if __name__ == '__main__':
    unittest.main()
