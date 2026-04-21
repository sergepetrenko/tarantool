## bugfix/datetime

* Fixed an assertion failure on an ambiguous case where day of year (`yday`,
  which defines calendar month and month day implicitly) and calendar month
  (without a month day) were both defined in the date text. Now such case is
  detected an an error is thrown (gh-11347).
