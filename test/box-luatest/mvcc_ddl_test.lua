local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_each(function(cg)
    cg.server = server:new{box_cfg = {memtx_use_mvcc_engine = true}}
    cg.server:start()
end)

g.after_each(function(cg)
    cg.server:stop()
end)

-- The test checks that all delete statements are handled correctly
-- on space drop
g.test_drop_space_many_delete_statements = function(cg)
    cg.server:exec(function()
        local txn_proxy = require("test.box.lua.txn_proxy")

        -- Create space with tuples
        local s = box.schema.space.create('test')
        s:create_index('pk')
        for i = 1, 100 do
            s:replace{i}
        end

        -- Delete the tuples concurrently
        local tx1 = txn_proxy:new()
        local tx2 = txn_proxy:new()
        tx1:begin()
        tx2:begin()
        for i = 1, 100 do
            local stmt = "box.space.test:delete{" .. i .. "}"
            tx1(stmt)
            tx2(stmt)
        end
        s:drop()
        tx1:rollback()
        tx2:rollback()

        -- Collect garbage
        box.internal.memtx_tx_gc(1000)
    end)
end

-- The test checks if background build of index does not crash when
-- MVCC is enabled
-- gh-10147
g.test_background_build = function(cg)
    cg.server:exec(function()
        local txn_proxy = require("test.box.lua.txn_proxy")
        local fiber = require('fiber')

        -- Create space with tuples
        local s = box.schema.space.create('test')
        s:create_index('pk')
        for i = 1, 2000 do
            s:replace{i}
        end

        local index_built = false
        local f = fiber.create(function()
            s:create_index('sk')
            index_built = true
        end)
        f:set_joinable(true)

        -- Delete the tuples concurrently
        local tx1 = txn_proxy:new()
        tx1:begin()
        for i = 1, 2000 do
            local stmt = "box.space.test:delete{" .. i .. "}"
            tx1(stmt)
        end

        assert(not index_built)
        local ok = f:join()
        t.assert(ok)
        local res = tx1:commit()
        -- Must be aborted by DDL
        t.assert_equals(res,
            {{error = "Transaction has been aborted by conflict"}})

        -- Collect garbage
        box.internal.memtx_tx_gc(1000)
    end)
end