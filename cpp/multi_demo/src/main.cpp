#include <iostream>
#include <vector>
#include <set>
#include <algorithm>
using namespace std;

// ================= 你的原始代码，一字未改 =================
class Solution {
public:
    vector<vector<int>> threeSum(vector<int>& nums) {
        set<vector<int>> set_ans;
        vector<vector<int>> ans;
        sort(nums.begin(),nums.end());
        for (int i = 0; i < nums.size(); i++) {
            if (i>0 && nums[i] == nums[i-1]) {
                continue;
            }
            int j = i+1;
            int k = nums.size()-1;
            while(j<k){
                if(nums[i]+nums[j]+nums[k]<0){
                    j++;
                }
                else if(nums[i]+nums[j]+nums[k]>0){
                    k++;
                }
                else if(nums[i]+nums[j]+nums[k]==0){
                    ans.push_back({nums[i],nums[j],nums[k]});
                }
            }
        }
        return ans;
    }
}
;
// =========================================================

void print(const vector<int>& v) {
    cout << "[";
    for (size_t i = 0; i < v.size(); i++) {
        cout << v[i];
        if (i + 1 < v.size()) cout << ", ";
    }
    cout << "]\n";
}

void run(vector<int> nums) {
    cout << "input : ";
    print(nums);
    vector<vector<int>> ans = Solution().threeSum(nums);
    cout << "output: ";
    for(auto& p : ans){
        print(p);
    }
    cout << "------------------------\n";
}

int main() {
    // 正常用例
    run({-1,0,1,2,-1,-4}); 
    // 没有 0，最容易触发越界
    run({0, 0, 0, 0});
    // 触发顺序错乱
    return 0;
}