// Test to make sure that tag ranges get split when full keys are used for the tag ranges
(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 1});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

    assert.eq(1, s.config.chunks.count());

    s.addShardTag("shard0000", "a");

    s.addTagRange("test.foo", {_id: 5}, {_id: 10}, "a");
    s.addTagRange("test.foo", {_id: 10}, {_id: 15}, "b");

    // Start the balancer and wait for it to do one round, which should split the chunks
    // appropriately
    s.startBalancer();
    s.awaitBalancerRound();
    s.stopBalancer();

    s.printShardingStatus();

    assert.eq(3, s.config.chunks.count(), 'Split did not occur.');

    s.stop();
})();
