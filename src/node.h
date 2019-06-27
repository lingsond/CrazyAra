/*
 * CrazyAra, a deep learning chess variant engine
 * Copyright (C) 2018 Johannes Czech, Moritz Willig, Alena Beyer
 * Copyright (C) 2019 Johannes Czech
 *
 * CrazyAra is free software: You can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * @file: node.h
 * Created on 13.05.2019
 * @author: queensgambit
 *
 * Class which stores the statistics of all nodes and in the search tree.
 */

#ifndef NODE_H
#define NODE_H

#include <mutex>
#include "position.h"
#include "movegen.h"
#include "board.h"

#include <blaze/Math.h>

using blaze::HybridVector;
using blaze::DynamicVector;
#include <iostream>

class Node
{
private:
    std::mutex mtx;
    float value;
    Board pos;
//    StateInfo state;
    DynamicVector<float> policyProbSmall;
    DynamicVector<float> childNumberVisits;
    DynamicVector<float> actionValues;
    DynamicVector<float> qValues;
    DynamicVector<float> scoreValues;
//    DynamicVector<float> waitForNNResults;

    // dummy
    DynamicVector<float> ones;

    std::vector<Move> legalMoves;
    int nbLegalMoves;
    bool isTerminal;
//    unsigned int numberWaitingChildNodes;
    unsigned int nbDirectChildNodes;

    float initialValue;
    int numberVisits  = 0;
    std::vector<Node*> childNodes;

    Node *parentNode;
    unsigned int childIdxOfParent;
    bool hasNNResults;
public:
//    Node();
    Node(Board pos,
         Node *parentNode,
         unsigned int childIdxOfParent);
    void setNeuralNetResults(float &value, DynamicVector<float>& policyProbSmall);
    DynamicVector<float> getPVecSmall() const;
    void setPVecSmall(const DynamicVector<float> &value);
    std::vector<Move> getLegalMoves() const;
    void setLegalMoves(const std::vector<Move> &value);
    void apply_virtual_loss_to_child(unsigned int childIdx, float virtualLoss);
    float getValue() const;
    void setValue(float value);
    size_t select_child_node(float cpuct);
    Node* get_child_node(size_t childIdx);
    void set_child_node(size_t childIdx, Node *newNode);

    /**
     * @brief backup_value Iteratively backpropagates a value prediction across all of the parents for this node.
     * The value is flipped at every ply.
     * @param value Value evaluation to backup, this is the NN eval in the general case or can be from a terminal node
     */
    void backup_value(unsigned int childIdx, float virtualLoss, float value);

    /**
     * @brief revert_virtual_loss_and_update Revert the virtual loss effect and apply the backpropagated value of its child node
     * @param child_idx Index to the child node to update
     * @param virtualLoss Specifies the virtual loss
     * @param value Specifies the value evaluation to backpropagate
     */
    void revert_virtual_loss_and_update(unsigned int child_idx, float virtualLoss, float value);

    /**
     * @brief backup_collision Iteratively removes the virtual loss of the collision event that occured
     * @param childIdx Index to the child node to update
     * @param virtualLoss  Specifies the virtual loss
     */
    void backup_collision(unsigned int childIdx, float virtualLoss);

    /**
     * @brief revert_virtual_loss Reverts the virtual loss for a target node
     * @param child_idx Index to the child node to update
     * @param virtualLoss  Specifies the virtual loss
     */
    void revert_virtual_loss(unsigned int childIdx, float virtualLoss);

    /**
     * @brief make_to_root Makes the node to the current root node by setting its parent to a nullptr
     */
    void make_to_root();

    friend class SearchThread;
    friend class MCTSAgent;

    DynamicVector<float> getPolicyProbSmall();
    void setPolicyProbSmall(const DynamicVector<float> &value);

    void get_mcts_policy(const float qValueWeight, const float q_value_min_visit_fac, DynamicVector<float>& mctsPolicy);
    DynamicVector<float> getQValues() const;

    void apply_dirichlet_noise_to_prior_policy(const float epsilon, const float alpha);

    void setQValues(const DynamicVector<float> &value);
    DynamicVector<float> getChildNumberVisits() const;
    unsigned int getNbDirectChildNodes() const;
};

extern std::ostream& operator<<(std::ostream& os, const Node *node);


#endif // NODE_H
