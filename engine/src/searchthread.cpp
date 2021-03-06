/*
  CrazyAra, a deep learning chess variant engine
  Copyright (C) 2018       Johannes Czech, Moritz Willig, Alena Beyer
  Copyright (C) 2019-2020  Johannes Czech

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * @file: searchthread.cpp
 * Created on 23.05.2019
 * @author: queensgambit
 */

#include "searchthread.h"
#ifdef TENSORRT
#include "NvInfer.h"
#include <cuda_runtime_api.h>
#include "common.h"
#endif

#include "inputrepresentation.h"
#include "outputrepresentation.h"
#include "util/blazeutil.h"
#include "uci.h"

SearchThread::SearchThread(NeuralNetAPI *netBatch, SearchSettings* searchSettings, MapWithMutex* mapWithMutex):
    netBatch(netBatch), isRunning(false), mapWithMutex(mapWithMutex), searchSettings(searchSettings)
{
    // allocate memory for all predictions and results
#ifdef TENSORRT
    CHECK(cudaMallocHost((void**) &inputPlanes, searchSettings->batchSize * NB_VALUES_TOTAL * sizeof(float)));
    CHECK(cudaMallocHost((void**) &valueOutputs, searchSettings->batchSize * sizeof(float)));
    CHECK(cudaMallocHost((void**) &probOutputs, netBatch->get_policy_output_length() * sizeof(float)));
#else
    inputPlanes = new float[searchSettings->batchSize * NB_VALUES_TOTAL];
    valueOutputs = new float[searchSettings->batchSize];
    probOutputs = new float[netBatch->get_policy_output_length()];
#endif
    searchLimits = nullptr;  // will be set by set_search_limits() every time before go()

    newNodes = make_unique<FixedVector<Node*>>(searchSettings->batchSize);
    newNodeSideToMove = make_unique<FixedVector<Color>>(searchSettings->batchSize);
    transpositionNodes = make_unique<FixedVector<Node*>>(searchSettings->batchSize*2);
    collisionNodes = make_unique<FixedVector<Node*>>(searchSettings->batchSize);
}

SearchThread::~SearchThread()
{
#ifdef TENSORRT
    CHECK(cudaFreeHost(inputPlanes));
    CHECK(cudaFreeHost(valueOutputs));
    CHECK(cudaFreeHost(probOutputs));
#else
    delete [] inputPlanes;
    delete [] valueOutputs;
    delete [] probOutputs;
#endif
}

void SearchThread::set_root_node(Node *value)
{
    rootNode = value;
}

void SearchThread::set_search_limits(SearchLimits *s)
{
    searchLimits = s;
}

bool SearchThread::is_running() const
{
    return isRunning;
}

void SearchThread::set_is_running(bool value)
{
    isRunning = value;
}

void SearchThread::add_new_node_to_tree(Board* newPos, Node* parentNode, size_t childIdx, bool inCheck)
{
    mapWithMutex->mtx.lock();
    unordered_map<Key, Node*>::const_iterator it = mapWithMutex->hashTable.find(newPos->hash_key());
    mapWithMutex->mtx.unlock();
    if(searchSettings->useTranspositionTable && it != mapWithMutex->hashTable.end() &&
            is_transposition_verified(it, newPos->get_state_info())) {
        Node *newNode = new Node(*it->second);
        parentNode->add_transposition_child_node(newNode, childIdx);
        parentNode->increment_no_visit_idx();
        transpositionNodes->add_element(newNode);
    }
    else {
        parentNode->increment_no_visit_idx();
        assert(parentNode != nullptr);
        Node *newNode = new Node(newPos, inCheck, parentNode, childIdx, searchSettings);
        // fill a new board in the input_planes vector
        // we shift the index by NB_VALUES_TOTAL each time
        board_to_planes(newPos, newPos->number_repetitions(), true, inputPlanes+newNodes->size()*NB_VALUES_TOTAL);

        // connect the Node to the parent
        parentNode->add_new_child_node(newNode, childIdx);

        // save a reference newly created list in the temporary list for node creation
        // it will later be updated with the evaluation of the NN
        newNodes->add_element(newNode);
        newNodeSideToMove->add_element(newPos->side_to_move());
    }
}

void SearchThread::stop()
{
    isRunning = false;
}

Node *SearchThread::get_root_node() const
{
    return rootNode;
}

SearchLimits *SearchThread::get_search_limits() const
{
    return searchLimits;
}

Node* get_new_child_to_evaluate(Board* pos, Node* rootNode, size_t& childIdx, NodeDescription& description, bool& inCheck, StateListPtr& states,  const SearchSettings* searchSettings)
{
    Node* currentNode = rootNode;
    description.depth = 0;
    states = StateListPtr(new std::deque<StateInfo>(0)); // Clear old list from memory and create a new one

    while (true) {
        currentNode->lock();
        childIdx = currentNode->select_child_node(searchSettings);
        currentNode->apply_virtual_loss_to_child(childIdx, searchSettings->virtualLoss);

        Node* nextNode = currentNode->get_child_node(childIdx);
        description.depth++;
        if (nextNode == nullptr) {
            description.isCollision = false;
            description.isTerminal = false;
            currentNode->unlock();
            inCheck = pos->gives_check(currentNode->get_move(childIdx));
            // this new StateInfo will be freed from memory when 'pos' is freed
            pos->do_move(currentNode->get_move(childIdx), *(new StateInfo));
            return currentNode;
        }
        if (nextNode->is_terminal()) {
            description.isCollision = false;
            description.isTerminal = true;
            currentNode->unlock();
            pos->do_move(currentNode->get_move(childIdx), *(new StateInfo));
            return currentNode;
        }
        if (!nextNode->has_nn_results()) {
            description.isCollision = true;
            description.isTerminal = false;
            currentNode->unlock();
            pos->do_move(currentNode->get_move(childIdx), *(new StateInfo));
            return currentNode;
        }
        currentNode->unlock();
        states->emplace_back();
        pos->do_move(currentNode->get_move(childIdx), states->back());
        currentNode = nextNode;
    }
}

void SearchThread::set_root_pos(Board *value)
{
    rootPos = value;
}

size_t SearchThread::get_tb_hits() const
{
    return tbHits;
}

void SearchThread::reset_tb_hits()
{
    tbHits = 0;
}

void fill_nn_results(size_t batchIdx, bool is_policy_map, const float* valueOutputs, const float* probOutputs, Node *node, size_t& tbHits, Color sideToMove, const SearchSettings* searchSettings)
{
    node->set_probabilities_for_moves(get_policy_data_batch(batchIdx, probOutputs, is_policy_map), get_current_move_lookup(sideToMove));
    node_post_process_policy(node, searchSettings->nodePolicyTemperature, is_policy_map, searchSettings);
    node_assign_value(node, valueOutputs, tbHits, batchIdx);
    node->enable_has_nn_results();
}

void SearchThread::set_nn_results_to_child_nodes()
{
    size_t batchIdx = 0;
    for (auto node: *newNodes) {
        if (!node->is_terminal()) {
            fill_nn_results(batchIdx, netBatch->is_policy_map(), valueOutputs, probOutputs, node, tbHits, newNodeSideToMove->get_element(batchIdx), searchSettings);
        }
        ++batchIdx;
        mapWithMutex->mtx.lock();
        mapWithMutex->hashTable.insert({node->hash_key(), node});
        mapWithMutex->mtx.unlock();
    }
}

void SearchThread::backup_value_outputs()
{
    backup_values(newNodes.get(), searchSettings->virtualLoss);
    newNodeSideToMove->reset_idx();
    backup_values(transpositionNodes.get(), searchSettings->virtualLoss);
}

void SearchThread::backup_collisions()
{
    for (auto node: *collisionNodes) {
        node->get_parent_node()->backup_collision(node->get_child_idx_for_parent(), searchSettings->virtualLoss);
    }
    collisionNodes->reset_idx();
}

bool SearchThread::nodes_limits_ok()
{
    return searchLimits->nodes == 0 || (rootNode->get_visits() - rootNode->get_terminal_visits() < searchLimits->nodes);
}

bool SearchThread::is_root_node_unsolved()
{
    return rootNode->get_node_type() == UNSOLVED;
}

void SearchThread::create_mini_batch()
{
    // select nodes to add to the mini-batch
    Node *parentNode;
    NodeDescription description;
    size_t childIdx;
    size_t numTerminalNodes = 0;

    while (!newNodes->is_full() &&
           !collisionNodes->is_full() &&
           !transpositionNodes->is_full() &&
           numTerminalNodes < TERMINAL_NODE_CACHE) {

        Board newPos = Board(*rootPos);
        bool inCheck;
        parentNode = get_new_child_to_evaluate(&newPos, rootNode, childIdx, description, inCheck, states, searchSettings);

        if(description.isTerminal) {
            ++numTerminalNodes;
            parentNode->backup_value(childIdx, -parentNode->get_child_node(childIdx)->get_value(), searchSettings->virtualLoss);
        }
        else if (description.isCollision) {
            // store a pointer to the collision node in order to revert the virtual loss of the forward propagation
            collisionNodes->add_element(parentNode->get_child_node(childIdx));
        }
        else {
            add_new_node_to_tree(&newPos, parentNode, childIdx, inCheck);
        }
    }
}

void SearchThread::thread_iteration()
{
    create_mini_batch();
    if (newNodes->size() != 0) {
        netBatch->predict(inputPlanes, valueOutputs, probOutputs);
        set_nn_results_to_child_nodes();
    }
    backup_value_outputs();
    backup_collisions();
}

void run_search_thread(SearchThread *t)
{
    t->set_is_running(true);
    t->reset_tb_hits();
    while(t->is_running() && t->nodes_limits_ok() && t->is_root_node_unsolved()) {
        t->thread_iteration();
    }
    t->set_is_running(false);
}

void backup_values(FixedVector<Node*>* nodes, float virtualLoss)
{
    for (auto node: *nodes) {
        node->get_parent_node()->backup_value(node->get_child_idx_for_parent(), -node->get_value(), virtualLoss);
    }
    nodes->reset_idx();
}

void node_assign_value(Node *node, const float* valueOutputs, size_t& tbHits, size_t batchIdx)
{
    if (!node->is_tablebase()) {
        node->set_value(valueOutputs[batchIdx]);
    }
    else {
        ++tbHits;
        if (node->get_value() != 0 && node->get_parent_node() != nullptr && node->get_parent_node()->is_tablebase()) {
            // use the average of the TB entry and NN eval for non-draws
            node->set_value((valueOutputs[batchIdx] + node->get_value()) * 0.5f);
        }
    }
}

void node_post_process_policy(Node *node, float temperature, bool isPolicyMap, const SearchSettings* searchSettings)
{
    if (!isPolicyMap) {
        node->apply_softmax_to_policy();
    }
    node->enhance_moves(searchSettings);
    node->apply_temperature_to_prior_policy(temperature);
}

bool is_transposition_verified(const unordered_map<Key,Node*>::const_iterator& it, const StateInfo* stateInfo) {
    return  it->second->has_nn_results() &&
            it->second->plies_from_null() == stateInfo->pliesFromNull &&
            stateInfo->repetition == 0;
}

