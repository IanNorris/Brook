import unittest
from bootstrap_perf_report import summarize_block_io
HEADER='dev waits reqs waitNsTotal waitNsMax spinIters legacy batch sg avgReqPerWait avgWaitNs avgSpin flushSubmitted flushErrors\n'
def table(waits=2,reqs=8,total=100,maximum=80,errors=0):
    return HEADER+f'virtio0 {waits} {reqs} {total} {maximum} 40 1 1 0 4 50 20 2 {errors}\n'
class BlockCounters(unittest.TestCase):
    def parse(self,before,after):
        return summarize_block_io('BOOTSTRAP_BEGIN compile\n'+before+'compiler output\n'+after+'BOOTSTRAP_END compile 5\n')['stages'][0]
    def test_delta_and_highwater_are_distinct(self):
        row=self.parse(table(),table(4,14,200,80))['devices']['virtio0']
        self.assertEqual(row['delta']['reqs'],6);self.assertEqual(row['delta']['waits'],2)
        self.assertEqual(row['mean_wait_ns'],50);self.assertIsNone(row['new_max_wait_ns'])
        self.assertEqual(row['cumulative_max_wait_ns_after'],80)
    def test_new_max_and_zero_waits(self):
        row=self.parse(table(),table(2,8,100,90))['devices']['virtio0']
        self.assertEqual(row['new_max_wait_ns'],90);self.assertIsNone(row['mean_wait_ns'])
    def test_counter_reset_is_not_negative_work(self):
        row=self.parse(table(),table(1))['devices']['virtio0']
        self.assertIn('unavailable',row);self.assertNotIn('delta',row)
    def test_missing_extra_or_incomplete_table_is_explicit(self):
        for after in ['',table()+table(),'dev waits reqs old\nvirtio0 3 8 1\n']:
            row=self.parse(table(),after)
            self.assertTrue('unavailable' in row or 'unavailable' in row['devices']['virtio0'])
    def test_unfinished_stage_is_not_reported_as_complete(self):
        self.assertEqual(summarize_block_io('BOOTSTRAP_BEGIN compile\n'+table())['stages'],[])
    def test_device_inventory_change_is_not_silent(self):
        row=self.parse(table(),table().replace('virtio0','virtio1'))
        self.assertEqual(set(row['devices']),{'virtio0','virtio1'})
        self.assertTrue(all('unavailable' in x for x in row['devices'].values()))
if __name__=='__main__':unittest.main()
