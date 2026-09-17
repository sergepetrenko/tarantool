local t = require('luatest')
local replica_set = require('luatest.replica_set')
local fio = require('fio')

local g = t.group()

g.before_each(function(cg)
    cg.replica_set = replica_set:new{}
    cg.master = cg.replica_set:build_and_add_server{
        alias = 'master',
        box_cfg = {checkpoint_count = 1, wal_cleanup_delay = 0},
    }
    cg.replica = cg.replica_set:build_and_add_server{
        alias = 'replica',
        box_cfg = {replication = cg.master.net_box_uri, read_only = true},
    }
    cg.replica_set:start()
    cg.master:exec(function()
        box.schema.space.create('test'):create_index('pk')
        box.schema.space.create('local_test', {is_local = true})
            :create_index('pk')
    end)
    cg.replica:wait_for_vclock_of(cg.master)
end)

g.after_each(function(cg)
    cg.replica_set:drop()
end)

g.test_gc_after_xlog_gap = function(cg)
    local master_id = cg.master:get_instance_id()
    local replica_uuid = cg.replica:get_instance_uuid()
    cg.replica:exec(function() box.snapshot() end)
    local old_vclock = cg.replica:get_vclock()
    local old_xlogs = fio.glob(fio.pathjoin(cg.master.workdir, '*.xlog'))
    t.assert_gt(#old_xlogs, 0)
    cg.master:exec(function()
        box.space.test:replace{1}
        box.snapshot()
        -- Opening another WAL lets the relay advance the consumer.
        box.space.test:replace{2}
    end)
    cg.replica:wait_for_vclock_of(cg.master)
    t.helpers.retrying({}, function()
        for _, path in ipairs(old_xlogs) do
            t.assert_not(fio.path.exists(path))
        end
    end)
    local gc_before = cg.master:exec(function() return box.info.gc().vclock end)
    t.assert_gt(gc_before[master_id], old_vclock[master_id])

    -- Lose the replica's updates since its snapshot. Recover without an
    -- upstream to prevent automatic rebootstrap from hiding the WAL gap.
    cg.replica:stop()
    for _, path in ipairs(fio.glob(cg.replica.workdir .. '/*.xlog')) do
        t.assert(fio.unlink(path))
    end
    cg.replica.box_cfg.replication = {}
    cg.replica:start()
    t.assert_equals(cg.replica:get_vclock()[master_id], old_vclock[master_id])

    for _ = 1, 2 do
        cg.replica:update_box_cfg{
            replication = cg.master.net_box_uri,
            replication_connect_quorum = 0,
        }
        cg.replica:exec(function(id)
            t.helpers.retrying({}, function()
                local message = box.info.replication[id].upstream.message or ''
                t.assert_str_contains(message, 'Missing .xlog')
            end)
            box.cfg{replication = {}}
        end, {master_id})
        cg.master:exec(function(uuid)
            t.helpers.retrying({}, function()
                t.assert_equals(box.info.gc().consumers, {})
            end)
            t.assert_not_equals(box.space._gc_consumers:get{uuid}, nil)
        end, {replica_uuid})
    end

    local xlogs = fio.glob(fio.pathjoin(cg.master.workdir, '*.xlog'))
    cg.master:exec(function(gc_before)
        -- A large local component used to mask the replicated component
        -- going backwards when GC compared vclock sums.
        local count = box.info.gc().signature + 1
        for i = 1, count do box.space.local_test:replace{i} end
        box.snapshot()
        local gc_vclock = box.info.gc().vclock
        t.assert_ge(gc_vclock[box.info.id], gc_before[box.info.id])
        t.assert_equals(gc_vclock[box.info.id], box.info.lsn)
    end, {gc_before})
    t.helpers.retrying({}, function()
        for _, path in ipairs(xlogs) do
            t.assert_not(fio.path.exists(path))
        end
    end)

    -- After rebootstrap the same consumer must protect WALs again.
    cg.replica.box_cfg.replication = cg.master.net_box_uri
    cg.replica:restart()
    cg.replica:wait_for_vclock_of(cg.master)
    cg.master:wait_for_downstream_to(cg.replica)
    cg.master:exec(function(uuid)
        local consumers = box.info.gc().consumers
        t.assert_equals(#consumers, 1)
        t.assert_equals(consumers[1].name, 'replica ' .. uuid)
        t.assert_equals(consumers[1].vclock[box.info.id], box.info.lsn)
    end, {replica_uuid})
end
