//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"
#include "rocksdb/env.h"
#include "rocksdb/db.h"
#include "util/testharness.h"
#include "utilities/merge_operators.h"

#include <algorithm>
#include <vector>
#include <string>

namespace rocksdb {

using namespace std;

class ColumnFamilyTest {
 public:
  ColumnFamilyTest() {
    env_ = Env::Default();
    dbname_ = test::TmpDir() + "/column_family_test";
    db_options_.create_if_missing = true;
    DestroyDB(dbname_, Options(db_options_, column_family_options_));
  }

  void Close() {
    delete db_;
    db_ = nullptr;
  }

  Status Open(vector<string> cf) {
    vector<ColumnFamilyDescriptor> column_families;
    for (auto x : cf) {
      column_families.push_back(
          ColumnFamilyDescriptor(x, column_family_options_));
    }
    return DB::OpenWithColumnFamilies(db_options_, dbname_, column_families,
                                      &handles_, &db_);
  }

  void Destroy() {
    delete db_;
    db_ = nullptr;
    ASSERT_OK(DestroyDB(dbname_, Options(db_options_, column_family_options_)));
  }

  void CreateColumnFamilies(const vector<string>& cfs) {
    int cfi = handles_.size();
    handles_.resize(cfi + cfs.size());
    for (auto cf : cfs) {
      ASSERT_OK(db_->CreateColumnFamily(column_family_options_, cf,
                                        &handles_[cfi++]));
    }
  }

  Status Put(int cf, const string& key, const string& value) {
    return db_->Put(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }
  Status Merge(int cf, const string& key, const string& value) {
    return db_->Merge(WriteOptions(), handles_[cf], Slice(key), Slice(value));
  }

  string Get(int cf, const string& key) {
    ReadOptions options;
    options.verify_checksums = true;
    string result;
    Status s = db_->Get(options, handles_[cf], Slice(key), &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  void CopyFile(const string& source, const string& destination,
                uint64_t size = 0) {
    const EnvOptions soptions;
    unique_ptr<SequentialFile> srcfile;
    ASSERT_OK(env_->NewSequentialFile(source, &srcfile, soptions));
    unique_ptr<WritableFile> destfile;
    ASSERT_OK(env_->NewWritableFile(destination, &destfile, soptions));

    if (size == 0) {
      // default argument means copy everything
      ASSERT_OK(env_->GetFileSize(source, &size));
    }

    char buffer[4096];
    Slice slice;
    while (size > 0) {
      uint64_t one = min(uint64_t(sizeof(buffer)), size);
      ASSERT_OK(srcfile->Read(one, &slice, buffer));
      ASSERT_OK(destfile->Append(slice));
      size -= slice.size();
    }
    ASSERT_OK(destfile->Close());
  }

  vector<ColumnFamilyHandle> handles_;
  ColumnFamilyOptions column_family_options_;
  DBOptions db_options_;
  string dbname_;
  DB* db_;
  Env* env_;
};

TEST(ColumnFamilyTest, AddDrop) {
  ASSERT_OK(Open({"default"}));
  ColumnFamilyHandle handles[4];
  ASSERT_OK(
      db_->CreateColumnFamily(column_family_options_, "one", &handles[0]));
  ASSERT_OK(
      db_->CreateColumnFamily(column_family_options_, "two", &handles[1]));
  ASSERT_OK(
      db_->CreateColumnFamily(column_family_options_, "three", &handles[2]));
  ASSERT_OK(db_->DropColumnFamily(handles[1]));
  ASSERT_OK(
      db_->CreateColumnFamily(column_family_options_, "four", &handles[3]));
  Close();
  ASSERT_TRUE(Open({"default"}).IsInvalidArgument());
  ASSERT_OK(Open({"default", "one", "three", "four"}));
  Close();

  vector<string> families;
  ASSERT_OK(DB::ListColumnFamilies(db_options_, dbname_, &families));
  sort(families.begin(), families.end());
  ASSERT_TRUE(families == vector<string>({"default", "four", "one", "three"}));
}

TEST(ColumnFamilyTest, ReadWrite) {
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"one", "two"});
  Close();
  ASSERT_OK(Open({"default", "one", "two"}));
  ASSERT_OK(Put(0, "foo", "v1"));
  ASSERT_OK(Put(0, "bar", "v2"));
  ASSERT_OK(Put(1, "mirko", "v3"));
  ASSERT_OK(Put(0, "foo", "v2"));
  ASSERT_OK(Put(2, "fodor", "v5"));

  for (int iter = 0; iter <= 3; ++iter) {
    ASSERT_EQ("v2", Get(0, "foo"));
    ASSERT_EQ("v2", Get(0, "bar"));
    ASSERT_EQ("v3", Get(1, "mirko"));
    ASSERT_EQ("v5", Get(2, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(0, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(1, "fodor"));
    ASSERT_EQ("NOT_FOUND", Get(2, "foo"));
    if (iter <= 1) {
      // reopen
      Close();
      ASSERT_OK(Open({"default", "one", "two"}));
    }
  }
  Close();
}

TEST(ColumnFamilyTest, IgnoreRecoveredLog) {
  string backup_logs = dbname_ + "/backup_logs";

  // delete old files in backup_logs directory
  env_->CreateDirIfMissing(backup_logs);
  vector<string> old_files;
  env_->GetChildren(backup_logs, &old_files);
  for (auto& file : old_files) {
    if (file != "." && file != "..") {
      env_->DeleteFile(backup_logs + "/" + file);
    }
  }

  column_family_options_.merge_operator =
      MergeOperators::CreateUInt64AddOperator();
  db_options_.wal_dir = dbname_ + "/logs";
  Destroy();
  ASSERT_OK(Open({"default"}));
  CreateColumnFamilies({"cf1", "cf2"});

  // fill up the DB
  string one, two, three;
  PutFixed64(&one, 1);
  PutFixed64(&two, 2);
  PutFixed64(&three, 3);
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(1, "mirko", one));
  ASSERT_OK(Merge(0, "foo", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(2, "fodor", one));
  ASSERT_OK(Merge(0, "bar", one));
  ASSERT_OK(Merge(2, "bla", one));
  ASSERT_OK(Merge(1, "mirko", two));
  ASSERT_OK(Merge(1, "franjo", one));

  // copy the logs to backup
  vector<string> logs;
  env_->GetChildren(db_options_.wal_dir, &logs);
  for (auto& log : logs) {
    if (log != ".." && log != ".") {
      CopyFile(db_options_.wal_dir + "/" + log, backup_logs + "/" + log);
    }
  }

  // recover the DB
  Close();

  // 1. check consistency
  // 2. copy the logs from backup back to WAL dir. if the recovery happens
  // again on the same log files, this should lead to incorrect results
  // due to applying merge operator twice
  // 3. check consistency
  for (int iter = 0; iter < 2; ++iter) {
    // assert consistency
    ASSERT_OK(Open({"default", "cf1", "cf2"}));
    ASSERT_EQ(two, Get(0, "foo"));
    ASSERT_EQ(one, Get(0, "bar"));
    ASSERT_EQ(three, Get(1, "mirko"));
    ASSERT_EQ(one, Get(1, "franjo"));
    ASSERT_EQ(one, Get(2, "fodor"));
    ASSERT_EQ(two, Get(2, "bla"));
    Close();

    if (iter == 0) {
      // copy the logs from backup back to wal dir
      for (auto& log : logs) {
        if (log != ".." && log != ".") {
          CopyFile(backup_logs + "/" + log, db_options_.wal_dir + "/" + log);
        }
      }
    }
  }
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  return rocksdb::test::RunAllTests();
}