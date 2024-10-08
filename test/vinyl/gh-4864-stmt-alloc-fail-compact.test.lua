test_run = require('test_run').new()
fiber = require('fiber')
digest = require('digest')

s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {run_count_per_level = 100, page_size = 128, range_size = 1024})

test_run:cmd("setopt delimiter ';'")
function dump(big)
    local step = big and 1 or 5
    for i = 1, 20, step do
        s:replace{i, digest.urandom(1000)}
    end
    box.snapshot()
end;

-- Tuple clean-up takes place after compaction is completed.
-- Meanwhile range count is updated during compaction process.
-- So instead of relying on range/run match, let's check explicitly
-- number of completed tasks.
--
function compact(tasks_expected)
    local scheduler = box.stat.vinyl().scheduler
    local tasks_completed = scheduler.tasks_completed
    s.index.pk:compact()
    repeat
        fiber.sleep(0.001)
    until box.stat.vinyl().scheduler.tasks_completed >= tasks_completed + tasks_expected
end;
test_run:cmd("setopt delimiter ''");

-- The first run should be big enough to prevent major compaction
-- on the next dump, because run_count_per_level is ignored on the
-- last level.
--
dump(true)
dump()
assert(s.index.pk:stat().range_count == 1)
assert(s.index.pk:stat().run_count == 2)

compact(1)
assert(s.index.pk:stat().range_count == 1)
assert(s.index.pk:stat().run_count == 1)

dump()
assert(s.index.pk:stat().range_count == 1)
assert(s.index.pk:stat().run_count == 2)

errinj = box.error.injection
errinj.set('ERRINJ_VY_STMT_ALLOC_COUNTDOWN', 0)
-- Should finish successfully despite vy_stmt_alloc() failure.
-- Still split_range() fails, as a result we get one range
-- instead two.
--
compact(1)
assert(s.index.pk:stat().range_count == 1)
assert(s.index.pk:stat().run_count == 1)
assert(errinj.get('ERRINJ_VY_STMT_ALLOC_COUNTDOWN') == -1)
errinj.set('ERRINJ_VY_STMT_ALLOC_COUNTDOWN', -1)

s:drop()

-- All the same except for delayed vy_stmt_alloc() fail.
-- Re-create space for the sake of test purity.
--
s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {run_count_per_level = 100, page_size = 128, range_size = 1024})

dump(true)
dump()

compact(1)

dump()

errinj = box.error.injection
errinj.set('ERRINJ_VY_STMT_ALLOC_COUNTDOWN', 5)
-- Compaction of first range fails, so it is re-scheduled and
-- then successfully finishes at the second attempt.
--
compact(2)
assert(s.index.pk:stat().range_count == 2)
assert(s.index.pk:stat().run_count == 2)
assert(errinj.get('ERRINJ_VY_STMT_ALLOC_COUNTDOWN') == -1)
errinj.set('ERRINJ_VY_STMT_ALLOC_COUNTDOWN', -1)
-- Unthrottle scheduler to allow next dump.
--
errinj.set("ERRINJ_VY_SCHED_TIMEOUT", 0.0001)

s:drop()

-- Once again but test that clean-up is made in case
-- vy_read_view_merge() fails.
--
s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {run_count_per_level = 100, page_size = 128, range_size = 1024})

dump(true)
dump()

compact(1)

dump()

errinj = box.error.injection
errinj.set('ERRINJ_VY_READ_VIEW_MERGE_FAIL', true)
compact(2)
assert(s.index.pk:stat().range_count == 2)
assert(s.index.pk:stat().run_count == 2)
assert(errinj.get('ERRINJ_VY_READ_VIEW_MERGE_FAIL') == false)
errinj.set('ERRINJ_VY_READ_VIEW_MERGE_FAIL', false)
s:drop()

-- Make sure that there's no extra format unref due to tuple
-- clean-up in the main thread. To achieve this let's sabotage
-- compaction process and delete all tuples: in case ref/unref
-- is the same, format will be deleted alongside with the last
-- tuple.
--
s = box.schema.space.create('test', {engine = 'vinyl'})
_ = s:create_index('pk', {run_count_per_level = 100, page_size = 128, range_size = 1024})

dump(true)
dump()

compact(1)

dump()
assert(s.index.pk:stat().range_count == 1)
assert(s.index.pk:stat().run_count == 2)

errinj.set('ERRINJ_VY_WRITE_ITERATOR_START_FAIL', true)
errinj.set("ERRINJ_VY_SCHED_TIMEOUT", 0.1)
compact(2)

-- Drop is required to unref all tuples.
--
s:drop()
-- After index is dropped, not all tuples are deallocated at once:
-- they may be still referenced (while being pushed) in Lua. So
-- invoke GC explicitly.
--
_ = collectgarbage("collect")

assert(errinj.get('ERRINJ_VY_WRITE_ITERATOR_START_FAIL') == false)
errinj.set('ERRINJ_VY_WRITE_ITERATOR_START_FAIL', false)
errinj.set("ERRINJ_VY_SCHED_TIMEOUT", 0)
