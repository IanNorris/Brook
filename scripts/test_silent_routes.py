#!/usr/bin/env python3
"""Check routing recovery never duplicates links or adopts foreign endpoints."""
import json
import unittest
from unittest.mock import patch
from linux_workload_reference import SilentRoutes


def node(identity,**props):
    return dict(id=identity,type='PipeWire:Interface:Node',info=dict(props=props))


def port(identity,node_id,channel,name):
    return dict(id=identity,type='PipeWire:Interface:Port',info=dict(props={
        'node.id':node_id,'audio.channel':channel,'port.name':name}))


def link(identity,source,target,state='active'):
    return dict(id=identity,type='PipeWire:Interface:Link',info=dict(state=state,props={
        'link.output.port':source,'link.input.port':target}))


class RoutingTests(unittest.TestCase):
    def fixture(self,existing=()):
        graph=[node(10,**{'node.name':'brook-null'}),
               node(20,**{'node.name':'qemu','application.process.id':'123','object.serial':100}),
               port(30,20,'FL','output_FL'),port(31,20,'FR','output_FR'),
               port(40,10,'FL','playback_FL'),port(41,10,'FR','playback_FR')]+list(existing)
        route=SilentRoutes.__new__(SilentRoutes);route.known=set();route.events=[];calls=[]
        def command(argv):
            calls.append(argv)
            if argv==['pw-dump']:return json.dumps(graph)
            if argv[:3]==['pw-cli','set-param','20']:return ''
            if argv[0]=='pw-link':return ''
            raise AssertionError(argv)
        route.command=command
        return route,graph,calls

    @patch('linux_workload_reference.time.sleep')
    def test_existing_stereo_links_are_adopted_without_recreation(self,_):
        route,_,calls=self.fixture([link(50,30,40),link(51,31,41)])
        route.update(123)
        self.assertFalse(any(c[0]=='pw-link' for c in calls))
        self.assertEqual([x['adopted'] for x in route.events[0]['links']],[True,True])
        route.update(123)
        self.assertEqual(len(route.events),1)

    @patch('linux_workload_reference.time.sleep')
    def test_missing_channel_is_created_and_foreign_links_not_adopted(self,_):
        route,_,calls=self.fixture([link(50,30,40),link(51,99,41)])
        route.update(123)
        self.assertEqual([c for c in calls if c[0]=='pw-link'],[['pw-link','-L','31','41']])
        self.assertEqual([x['adopted'] for x in route.events[0]['links']],[True,False])

    @patch('linux_workload_reference.time.sleep')
    def test_fresh_route_creates_both_channels(self,_):
        route,_,calls=self.fixture();route.update(123)
        self.assertEqual([c for c in calls if c[0]=='pw-link'],[
            ['pw-link','-L','30','40'],['pw-link','-L','31','41']])

    @patch('linux_workload_reference.time.sleep')
    def test_duplicate_or_failed_link_is_rejected(self,_):
        for links in ([link(50,30,40),link(51,30,40)],[link(50,30,40,'error')]):
            route,_,calls=self.fixture(links)
            with self.assertRaisesRegex(RuntimeError,'ambiguous or failed'):route.update(123)
            self.assertFalse(route.events);self.assertFalse(route.known)
            self.assertFalse(any(c[0]=='pw-link' for c in calls))

    @patch('linux_workload_reference.time.sleep')
    def test_foreign_process_node_is_untouched(self,_):
        route,_,calls=self.fixture();route.update(456)
        self.assertEqual(calls,[['pw-dump']]);self.assertFalse(route.events)


if __name__=='__main__':unittest.main()
