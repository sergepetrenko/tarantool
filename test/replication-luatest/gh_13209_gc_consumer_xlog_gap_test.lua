local t = require('luatest')
local replica_set = require('luatest.replica_set')
local fio = require('fio')

local g = t.group(nil, {{drop_consumer = false}, {drop_consumer = true}})

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
    t.tarantool.skip_if_not_debug()
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

    if cg.params.drop_consumer then
        cg.master:exec(function(uuid)
            box.space._gc_consumers:delete{uuid}
        end, {replica_uuid})
    end

    for _ = 1, 2 do
        cg.master:exec(function()
            box.error.injection.set('ERRINJ_RELAY_WAL_START_DELAY', true)
        end)
        cg.replica:update_box_cfg{
            replication = cg.master.net_box_uri,
            replication_connect_quorum = 0,
        }
        -- The consumer must be inactive before relay discovers the WAL gap.
        cg.replica:exec(function(id)
            t.helpers.retrying({}, function()
                t.assert_equals(box.info.replication[id].upstream.status, 'sync')
            end)
        end, {master_id})
        cg.master:exec(function()
            t.assert_equals(box.info.gc().consumers, {})
            box.error.injection.set('ERRINJ_RELAY_WAL_START_DELAY', false)
        end)
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

-- Cleanup may advance inside a WAL pinned by another consumer. A replica
-- subscribing at an earlier position in that file must still protect WALs.
g.test_subscribe_inside_retained_wal = function(cg)
    local pin = cg.replica_set:build_and_add_server{
        alias = 'pin',
        box_cfg = {
            replication = cg.master.net_box_uri,
            read_only = true,
            bootstrap_strategy = 'config',
            bootstrap_leader = cg.master.net_box_uri,
        },
    }
    pin:start()
    cg.replica:wait_for_vclock_of(cg.master)
    cg.replica:exec(function() box.snapshot() end)
    local old_vclock = cg.replica:get_vclock()
    local master_id = cg.master:get_instance_id()
    local replica_uuid = cg.replica:get_instance_uuid()
    cg.master:exec(function() box.space.test:replace{1} end)
    for _, replica in ipairs({pin, cg.replica}) do
        replica:wait_for_vclock_of(cg.master)
        -- SUBSCRIBE records the precise position inside the current WAL.
        replica:update_box_cfg{replication = {}}
        replica:update_box_cfg{replication = cg.master.net_box_uri}
        cg.master:wait_for_downstream_to(replica)
        replica:stop()
    end
    cg.master:exec(function(old_vclock)
        -- Keep the pinned position strictly inside the sealed WAL.
        box.space.test:replace{2}
        box.snapshot()
        t.assert_gt(box.info.gc().vclock[box.info.id],
                    old_vclock[box.info.id])
    end, {old_vclock})

    for _, path in ipairs(fio.glob(cg.replica.workdir .. '/*.xlog')) do
        t.assert(fio.unlink(path))
    end
    if cg.params.drop_consumer then
        cg.master:exec(function(uuid)
            box.space._gc_consumers:delete{uuid}
        end, {replica_uuid})
    end
    cg.replica.box_cfg.replication = {}
    cg.replica:start()
    t.assert_equals(cg.replica:get_vclock()[master_id], old_vclock[master_id])
    cg.replica:update_box_cfg{replication = cg.master.net_box_uri}
    cg.replica:wait_for_vclock_of(cg.master)
    cg.master:wait_for_downstream_to(cg.replica)
    cg.master:exec(function(uuid)
        for _, consumer in ipairs(box.info.gc().consumers) do
            if consumer.name == 'replica ' .. uuid then return end
        end
        t.fail('Replica GC consumer is missing')
    end, {replica_uuid})

    cg.master:exec(function()
        box.space.test:replace{2}
        box.snapshot()
        box.space.test:replace{3}
    end)
    cg.replica:wait_for_vclock_of(cg.master)
    cg.master:exec(function(uuid)
        local checkpoint = box.info.gc().checkpoints[1].vclock
        t.helpers.retrying({}, function()
            for _, consumer in ipairs(box.info.gc().consumers) do
                if consumer.name == 'replica ' .. uuid then
                    t.assert_ge(consumer.vclock[box.info.id],
                                checkpoint[box.info.id])
                    return
                end
            end
            t.fail('Replica GC consumer is missing')
        end)
    end, {replica_uuid})
end
