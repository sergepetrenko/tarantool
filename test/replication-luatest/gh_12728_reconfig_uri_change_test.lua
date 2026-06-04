local t = require('luatest')
local cluster = require('luatest.replica_set')

local g = t.group('gh-12728-reconfig-uri-change')

g.before_each(function()
    g.cluster = cluster:new({})
    g.master = g.cluster:build_server({alias = 'master'})
    g.cluster:add_server(g.master)
    g.master:start()
    -- Two distinct users to switch replication credentials between.
    g.master:exec(function()
        box.schema.user.create('user1', {password = 'pass1'})
        box.schema.user.grant('user1', 'replication')
        box.schema.user.create('user2', {password = 'pass2'})
        box.schema.user.grant('user2', 'replication')
    end)

    g.uri1 = 'user1:pass1@unix/:' .. g.master.net_box_uri
    g.uri2 = 'user2:pass2@unix/:' .. g.master.net_box_uri

    g.replica = g.cluster:build_server({
        alias = 'replica',
        box_cfg = {
            bootstrap_strategy = 'legacy',
            replication = g.uri1,
        },
    })
    g.cluster:add_server(g.replica)
    g.replica:start()
    g.replica:assert_follows_upstream(g.master:get_instance_id())
end)

g.after_each(function()
    g.cluster:drop()
end)

-- Reconfiguring with an identical URI must not interrupt the working applier:
-- the master relay loop should stay alive.
g.test_same_uri_keeps_connection = function()
    local master_id = g.master:get_instance_id()
    g.replica:exec(function(uri)
        box.cfg{replication = uri}
    end, {g.uri1})
    g.replica:assert_follows_upstream(master_id)
    t.assert_equals(g.master:grep_log('exiting the relay loop'), nil)
end

-- Reconfiguring with changed credentials must drop the old connection and
-- reconnect with the new URI (gh-12728).
g.test_changed_credentials_reconnects = function()
    local master_id = g.master:get_instance_id()
    local peer = g.replica:exec(function(id)
        return box.info.replication[id].upstream.peer
    end, {master_id})
    t.assert_str_contains(peer, 'user1')

    g.replica:exec(function(uri)
        box.cfg{replication = uri}
    end, {g.uri2})
    t.helpers.retrying({}, function()
        g.replica:assert_follows_upstream(master_id)
    end)

    -- The applier now uses the new credentials.
    peer = g.replica:exec(function(id)
        return box.info.replication[id].upstream.peer
    end, {master_id})
    t.assert_str_contains(peer, 'user2')

    -- The old connection was dropped, so the master's relay loop exited.
    t.helpers.retrying({}, function()
        t.assert_not_equals(
            g.master:grep_log('exiting the relay loop'), nil)
    end)
end
