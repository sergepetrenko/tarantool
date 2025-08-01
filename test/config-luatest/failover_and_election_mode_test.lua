-- tags: parallel

-- Verify all the compositions of possible replication.failover
-- and replication.election_mode values.
--
-- If the faliover mode is 'supervised' and the election mode is
-- set to a value that doesn't correspond to one deduced from
-- failover.replicasets.*.synchro_mode, the configuration is
-- considered incorrect.
--
-- If the election mode is set to a value other than null or 'off'
-- and the failover mode is not 'election' or 'supervised', the
-- configuration is considered incorrect.
--
-- All the other cases are allowed.

local t = require('luatest')
local cbuilder = require('luatest.cbuilder')
local cluster = require('test.config-luatest.cluster')
local textutils = require('internal.config.utils.textutils')

local g = t.group()

g.before_all(cluster.init)
g.after_each(cluster.drop)
g.after_all(cluster.clean)

-- An error that should appear if replication.failover =
-- supervised and replication.election_mode is set to a value that
-- is not the same as one deduced from
-- failover.replicasets.*.synchro_mode.
--
-- It a template and it should parametrized with election mode
-- supervised_error_t:format(election_mode).
local supervised_error_t = textutils.toline([[
    replication.election_mode = %q is set for instance
    "instance-004" of replicaset "replicaset-001" of group
    "group-001", but this option is to be deduced from
    failover.replicasets.replicaset-001.synchro_mode when
    replication.failover = "supervised"; the suggestion is to
    leave the replication.election_mode option unset
]])

-- An error that should appear if replication.failover and
-- replication.election_mode are conflicting.
--
-- It a template and it should parametrized with election mode and
-- failover values: error_t:format(election_mode, failover).
--
-- Nuances:
--
-- * In general, a missed value is shown as 'nil', while a null
--   value is shown as 'cdata<void *>: NULL' if there is no
--   default value in the schema.
-- * If replication.election_mode is missed or null, the message
--   can't appear.
-- * If replication.failover is missed or null, it is shown as
--   'off', because it is the default value.
local error_t = textutils.toline([[
    replication.election_mode = %q is set for instance "instance-004" of
    replicaset "replicaset-001" of group "group-001", but this option is
    only applicable if replication.failover = "election"; the replicaset is
    configured with replication.failover = %q; if this particular instance
    requires its own election mode, for example, if it is an anonymous
    replica, consider configuring the election mode specifically for this
    particular instance
]])

local function expected_error(failover, election_mode)
    if failover == 'supervised' then
        return supervised_error_t:format(election_mode)
    end

    -- The error message shows the replication.failover
    -- parameter by its default value if it is missed or null.
    --
    -- See comments for the `error_t` template for details.
    if failover == nil then
        failover = 'off'
    end

    return error_t:format(election_mode, failover)
end

-- Configure three usual instances and one with specific election
-- mode.
--
-- All the instances are in a replicaset with the given failover
-- mode.
local function build_config(failover, election_mode, synchro_mode, anon)
    local cb = cbuilder:new()
        :set_replicaset_option('replication.failover', failover)
        :add_instance('instance-001', {})
        :add_instance('instance-002', {})
        :add_instance('instance-003', {})
        :add_instance('instance-004', {
            replication = {
                election_mode = election_mode,
                anon = anon,
            },
        })

    if failover == nil or failover == 'off' then
        cb:set_instance_option('instance-001', 'database.mode', 'rw')
    end

    if failover == 'manual' then
        cb:set_replicaset_option('leader', 'instance-001')
    end

    if synchro_mode ~= nil then
        cb:set_global_option('failover.replicasets.replicaset-001.synchro_mode',
            synchro_mode)
    end

    return cb:config()
end

-- Verify that the given incorrect configuration is reported as
-- startup error.
local function failure_case(failover, election_mode, synchro_mode, anon)
    return function(g)
        local config = build_config(failover, election_mode, synchro_mode, anon)
        local exp_err = expected_error(failover, election_mode)
        cluster.startup_error(g, config, exp_err)
    end
end

-- Verify that the given correct configuration allows to start a
-- replicaset successfully.
local function success_case(failover, election_mode, synchro_mode, anon)
    return function(g)
        local config = build_config(failover, election_mode, synchro_mode, anon)
        local cluster = cluster.new(g, config)
        cluster:start()

        cluster['instance-004']:exec(function(failover, election_mode,
                                              synchro_mode, anon)
            -- The effective default value is 'off' except:
            --
            -- * 'candidate' if failover = election
            -- * 'manual' if failover = supervised and
            --   failover.replicasets.<...>.synchro_mode = true
            if election_mode == nil then
                if failover == 'election' then
                    election_mode = 'candidate'
                elseif failover == 'supervised' and
                       synchro_mode and
                       not anon then
                    election_mode = 'manual'
                else
                    election_mode = 'off'
                end
            end
            t.assert_equals(box.cfg.election_mode, election_mode)
        end, {failover, election_mode, synchro_mode, anon})
    end
end

-- Whether the given replication.failover value enables RAFT-based
-- leader election.
local failover_enables_election = {
    missed = false,
    null = false,
    off = false,
    manual = false,
    election = true,
    supervised = false,
}

-- Whether the given replication.election_mode value requires the
-- RAFT-based leader election be enabled.
local election_mode_requires_election = {
    missed = false,
    null = false,
    off = false,
    voter = true,
    manual = true,
    candidate = true,
}

-- The nil and box.NULL values are encoded as 'missed' and 'null'
-- in a test case parameters to use as a table keys. The function
-- decodes them back.
local function decode_missed_and_null(v)
    if v == 'missed' then
        return nil
    end
    if v == 'null' then
        return box.NULL
    end
    return v
end

-- Glue all the code above together and define test cases.
for failover_str, enabled in pairs(failover_enables_election) do
    for election_mode_str, required in pairs(election_mode_requires_election) do
        local expect_failure = required and not enabled
        local case_name = 'test_' .. table.concat({
            'failover', failover_str,
            'election_mode', election_mode_str,
            'expect', expect_failure and 'failure' or 'success',
        }, '_')

        local failover = decode_missed_and_null(failover_str)
        local election_mode = decode_missed_and_null(election_mode_str)

        if expect_failure then
            g[case_name] = failure_case(failover, election_mode)
        else
            g[case_name] = success_case(failover, election_mode)
        end
    end
end

-- Add test cases with
-- failover.replicasets.<replicaset_name>.synchro_mode = true.
for _, anon in ipairs({false, true}) do
    for election_mode_str, _ in pairs(election_mode_requires_election) do
        local synchro_mode = true
        local failover_str = 'supervised'
        local expect_failure
        if anon then
            expect_failure =
                election_mode_str ~= 'missed' and
                election_mode_str ~= 'null' and
                election_mode_str ~= 'off'
        else
            expect_failure =
                election_mode_str ~= 'missed' and
                election_mode_str ~= 'null' and
                election_mode_str ~= 'manual'
        end
        local case_name = 'test_' .. table.concat({
            'failover', failover_str,
            'election_mode', election_mode_str,
            'synchro_mode', tostring(synchro_mode),
            'anon', tostring(anon),
            'expect', expect_failure and 'failure' or 'success',
        }, '_')

        local failover = decode_missed_and_null(failover_str)
        local election_mode = decode_missed_and_null(election_mode_str)

        if expect_failure then
            g[case_name] = failure_case(failover, election_mode, synchro_mode,
                anon)
        else
            g[case_name] = success_case(failover, election_mode, synchro_mode,
                anon)
        end
    end
end
