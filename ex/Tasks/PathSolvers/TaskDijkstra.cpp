/* Generated by Together */

#include "TaskDijkstra.hpp"
#include "Dijkstra.hpp"
#include "Tasks/OrderedTask.h"
#include "Util.h"

unsigned invert_distance(const unsigned d, const unsigned nlayers)
{
  return -d;
}

TaskDijkstra::~TaskDijkstra() {
}


TaskDijkstra::TaskDijkstra(OrderedTask* _task, const unsigned task_size):
  task(_task),
  shortest(false),
  num_taskpoints(task_size)
{
  solution.reserve(num_taskpoints);
}


const SearchPoint &
TaskDijkstra::get_point(const ScanTaskPoint &sp) const
{
  return task->get_tp_search_points(sp.first)[sp.second];
}


unsigned TaskDijkstra::distance(const ScanTaskPoint &curNode,
                              const SearchPoint &currentLocation) const
{
  return get_point(curNode).flat_distance(currentLocation);
}

unsigned TaskDijkstra::distance(const ScanTaskPoint &s1,
                              const ScanTaskPoint &s2) const
{
  return get_point(s1).flat_distance(get_point(s2));
}

void TaskDijkstra::add_edges(Dijkstra<ScanTaskPoint> &dijkstra,
                             const ScanTaskPoint& curNode) 
{
  ScanTaskPoint destination;
  destination.first = curNode.first+1;

  const unsigned dsize = task->get_tp_search_points(destination.first).size();

  for (destination.second=0; 
       destination.second< dsize; destination.second++) {

    const unsigned dr = distance(curNode, destination);
    if (dr) {
      dijkstra.link(destination, shortest? dr: invert_distance(dr,1));
    }
  }
}


unsigned TaskDijkstra::distance_opt(const ScanTaskPoint &start,
                                    const bool req_shortest)
{
  shortest = req_shortest;

  ScanTaskPoint lastNode = start;
  Dijkstra<ScanTaskPoint> dijkstra(start);

  solution[start.first] = get_point(start);

  while (!dijkstra.empty()) {

    const ScanTaskPoint curNode = dijkstra.pop();

    if (curNode.first != lastNode.first) {
      solution[curNode.first] = get_point(curNode);
      lastNode = curNode;
    }

    if (curNode.first == num_taskpoints-1) {
      const unsigned d = dijkstra.dist();
      return shortest? d: invert_distance(d,curNode.first-start.first);
    }

    add_edges(dijkstra, curNode);
  }

  return -1; // No path found
}

unsigned 
TaskDijkstra::distance_opt_achieved(const SearchPoint &currentLocation,
                                    const bool req_shortest)
{
  shortest = false; // internally

  const ScanTaskPoint start(0,0);
  ScanTaskPoint lastNode(1000,1000);
  Dijkstra<ScanTaskPoint> dijkstra(start);

  const unsigned activeStage = task->getActiveTaskPointIndex();

  unsigned min_d = -1;
  unsigned min_d_actual = -1;
  unsigned max_d_actual = 0;

  while (!dijkstra.empty()) {

    const ScanTaskPoint curNode = dijkstra.pop();

    if (curNode.first != lastNode.first) {
      solution[curNode.first] = get_point(curNode);
      lastNode = curNode;
    }

    if (curNode.first != activeStage) {
      add_edges(dijkstra, curNode);
    } else {

      const unsigned d_acc = invert_distance(dijkstra.dist(),activeStage);

      TaskDijkstra inner_dijkstra(task, num_taskpoints);
      unsigned d_remaining = 0;

      if (curNode.first == num_taskpoints-1) {
        d_remaining = 0;
      } else {
        d_remaining = inner_dijkstra.distance_opt(curNode, req_shortest);
      }

      bool best=false;
      if (req_shortest) {
        // need to take into account distance from here to target

        const unsigned d_this = distance(curNode, currentLocation);

        if (d_remaining+d_this<min_d) {
          min_d = d_remaining+d_this; 
          min_d_actual = d_remaining+d_acc;
          best=true;
        }
      } else {
        // here we are only interested in scored distance
        if (d_remaining+d_acc>max_d_actual) {
          max_d_actual = d_remaining+d_acc;
          best=true;
        }
      }
      if (best) {
        solution[curNode.first] = get_point(curNode);

        for (unsigned j=activeStage+1; j<num_taskpoints; j++) {
          solution[j]= inner_dijkstra.solution[j];
        }
      }

    }
  }

  for (unsigned j=0; j<num_taskpoints; j++) {
    if (req_shortest) {
      task->set_tp_search_min(j, solution[j]);
    } else {
      task->set_tp_search_max(j, solution[j]);
    }
  }

  if (req_shortest) {
    return min_d_actual; 
  } else {
    return max_d_actual;
  }
}


/*
  TODO/incomplete 

  only scan parts that are required, and prune out points
  that become irrelevant (strictly under-performing) 

  if in sector, prune out all default points that result in
  lower distance than current achieved max

  if searching min 
    first search max actual up to current
      (taking into account aircraft location?)
    then search min after that from aircraft location

  also update saved rank for potential pruning operations

*/
