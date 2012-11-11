/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libgen.h>

#include <iostream>
#include <string>
#include <sstream>

#include <boost/lexical_cast.hpp>

#include <mesos/scheduler.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>
#include <stout/duration.hpp>
#include <stout/stopwatch.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/run.hpp>

#include <list>
#include <deque>

using namespace mesos;
using namespace process;

using process::wait;

using boost::lexical_cast;

using std::cout;
using std::cerr;
using std::endl;
using std::flush;
using std::string;
using std::vector;
using std::list;
using std::deque;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 32;

struct Task
{
  int taskID;
  Stopwatch lifetime;

  Task(const int taskID_) : 
    taskID(taskID_) {lifetime.start();}

  ~Task() {}
};

class ModelScheduler : public Scheduler
{
public:
  ModelScheduler(const ExecutorInfo& _executor)
    : executor(_executor),
      tasksLaunched(0),
      tasksGenned(0) {genTask();}

  virtual ~ModelScheduler() {}

  virtual void registered(SchedulerDriver*,
                          const FrameworkID&,
                          const MasterInfo&)
  {
    cout << "Registered!" << endl;
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) {}

  virtual void disconnected(SchedulerDriver* driver) {}

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    cout << "." << flush;
    // if no tasks available, return offers
    if (taskQueue.empty()) {
      for (size_t i = 0; i < offers.size(); i++) {
        const Offer& offer = offers[i];
        driver->declineOffer(offer.id());
      }
      cout << "No tasks launched because none were available" << endl;
      return;
    }

    sleep(3);
    for (size_t i = 0; i < offers.size(); i++) {
      if (taskQueue.empty()) break;

      const Offer& offer = offers[i];

      // Lookup resources we care about.
      // TODO(benh): It would be nice to ultimately have some helper
      // functions for looking up resources.
      double cpus = 0;
      double mem = 0;

      for (int i = 0; i < offer.resources_size(); i++) {
        const Resource& resource = offer.resources(i);
        if (resource.name() == "cpus" &&
            resource.type() == Value::SCALAR) {
          cpus = resource.scalar().value();
        } else if (resource.name() == "mem" &&
                   resource.type() == Value::SCALAR) {
          mem = resource.scalar().value();
        }
      }

      // Launch tasks (only one per offer).
      vector<TaskInfo> tasks;
      if (cpus >= CPUS_PER_TASK && mem >= MEM_PER_TASK) {
        Task task = taskQueue.front();
        taskQueue.pop_front();
        tasksInFlight.push_back(task);
        int taskId = task.taskID;

        cout << "Starting task " << taskId << " on "
             << offer.hostname() << endl;

        TaskInfo taskInfo;
        taskInfo.set_name("Task " + lexical_cast<string>(taskId));
        taskInfo.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        taskInfo.mutable_slave_id()->MergeFrom(offer.slave_id());
        taskInfo.mutable_executor()->MergeFrom(executor);

        Resource* resource;

        resource = taskInfo.add_resources();
        resource->set_name("cpus");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(CPUS_PER_TASK);

        resource = taskInfo.add_resources();
        resource->set_name("mem");
        resource->set_type(Value::SCALAR);
        resource->mutable_scalar()->set_value(MEM_PER_TASK);

        tasks.push_back(taskInfo);

        cpus -= CPUS_PER_TASK;
        mem -= MEM_PER_TASK;
      }

      driver->launchTasks(offer.id(), tasks);
    }
  }

  virtual void offerRescinded(SchedulerDriver* driver,
                              const OfferID& offerId) {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    int taskId = lexical_cast<int>(status.task_id().value());
    cout << "Task " << taskId << " is in state " << status.state() << endl;

    if (status.state() == TASK_FINISHED || status.state() == TASK_LOST)
    {
      list<Task>::iterator i;
      for(i = tasksInFlight.begin(); i != tasksInFlight.end(); ++i)
      {
        if (i->taskID == taskId) break;
      }

      assert(i != tasksInFlight.end());
      Task task = *i;
      tasksInFlight.erase(i);

      if(status.state() == TASK_FINISHED)
      {
        task.lifetime.stop();
        cout << "Task " << taskId << " finished at time " << task.lifetime.elapsed() << endl;
      } 
      else // now we know that the task is lost
      {
        taskQueue.push_front(task);
        cout << "Task " << taskId << " was placed back on the queue" << endl;
      }

    }
  }

  virtual void frameworkMessage(SchedulerDriver* driver,
                                const ExecutorID& executorId,
                                const SlaveID& slaveId,
                                const string& data) {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void executorLost(SchedulerDriver* driver,
                            const ExecutorID& executorId,
                            const SlaveID& slaveId,
                            int status) {}

  virtual void error(SchedulerDriver* driver, const string& message) {}

  void genTask()
  {
    taskQueue.push_back(Task(tasksGenned++));
    if(tasksGenned < 20)
    {
      delay(Seconds(1.0), self(),
            &ModelScheduler::genTask);
    }
  }

private:
  const ExecutorInfo executor;
  string uri;
  int tasksLaunched;
  int tasksGenned;
  deque<Task> taskQueue;
  list<Task> tasksInFlight;
};

int main(int argc, char** argv)
{
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <master>" << endl;
    return -1;
  }

  // Find this executable's directory to locate executor.
  string path = os::realpath(dirname(argv[0])).get();
  string uri = path + "/long-lived-executor";
  if (getenv("MESOS_BUILD_DIR")) {
    uri = string(getenv("MESOS_BUILD_DIR")) + "/src/long-lived-executor";
  }

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value(uri);

  ModelScheduler scheduler(executor);

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Model Framework (C++)");

  MesosSchedulerDriver driver(&scheduler, framework, argv[1]);

  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
